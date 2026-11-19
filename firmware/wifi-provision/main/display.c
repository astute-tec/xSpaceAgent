/*
 * Waveshare ESP32-S3-Touch-AMOLED-1.75C 显示 + 电源初始化
 * 引脚与初始化时序照搬 xiaozhi-esp32 的该板 BSP。
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_co5300.h"

#include "board.h"
#include "display.h"

static const char *TAG = "disp";

#define LCD_OPCODE_WRITE_CMD (0x02ULL)

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_axp;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static SemaphoreHandle_t s_flush_lock;
static uint16_t *s_dma_buf = NULL;
static SemaphoreHandle_t s_frame_lock;
static SemaphoreHandle_t s_trans_sem;   /* SPI 传输完成信号 */
static bool s_dma_pending;
static uint32_t s_full_generation;
static uint32_t s_drawn_frames;
static uint32_t s_failed_frames;

static const co5300_lcd_init_cmd_t vendor_init[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

static esp_err_t axp_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_axp, buf, sizeof(buf), 1000);
}

static void axp2101_init(void)
{
    /* 与 Waveshare BSP 的 Pmic 构造一致：只开 DC1(3.3V) + ALDO1(3.3V 给麦克风) */
    axp_write(0x22, 0x06);   /* PWRON 作为 PowerOff 源 */
    axp_write(0x27, 0x10);   /* 长按 4s 关机 */
    axp_write(0x80, 0x01);   /* 关闭除 DC1 外的 DCDC */
    axp_write(0x90, 0x00);   /* 关闭所有 LDO */
    axp_write(0x91, 0x00);
    axp_write(0x82, 18);     /* DC1 = 3.3V */
    axp_write(0x92, 28);     /* ALDO1 = 3.3V (MIC) */
    axp_write(0x90, 0x01);   /* 使能 ALDO1 */
    axp_write(0x64, 0x02);   /* 充电目标 4.1V */
    axp_write(0x61, 0x02);
    axp_write(0x62, 0x08);
    axp_write(0x63, 0x01);
}

void display_read_power(display_power_t *out)
{
    if (!out) return;
    memset(out,0,sizeof(*out)); out->percent=-1;
    if (!s_axp) return;
    uint8_t reg=0, status[2]={0};
    if (i2c_master_transmit_receive(s_axp,&reg,1,status,2,20)!=ESP_OK) return;
    out->battery_present=(status[0] & (1<<3))!=0;
    out->external_power=(status[0] & (1<<5))!=0;
    out->charging=((status[1]>>5)&3)==1;
    if (out->battery_present) {
        reg=0xa4; uint8_t percent=0xff;
        if (i2c_master_transmit_receive(s_axp,&reg,1,&percent,1,20)!=ESP_OK || percent>100) return;
        out->percent=percent;
    }
    out->valid=true;
}

/* SPI 颜色传输完成回调：唤醒 flush 循环等待者 */
static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_trans_sem, &hp);
    return hp == pdTRUE;
}

static void i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags = {.enable_internal_pullup = 1},
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_i2c_bus));

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x34,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev, &s_axp));
}

void display_brightness(uint8_t level)
{
    if (!s_io) return;
    uint8_t data[1] = {level};
    uint32_t cmd = (LCD_OPCODE_WRITE_CMD << 24) | (0x51 << 8);
    esp_lcd_panel_io_tx_param(s_io, cmd, data, sizeof(data));
}

esp_err_t display_init(void)
{
    i2c_init();
    axp2101_init();

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_PCLK,
        .data0_io_num = PIN_LCD_D0,
        .data1_io_num = PIN_LCD_D1,
        .data2_io_num = PIN_LCD_D2,
        .data3_io_num = PIN_LCD_D3,
        .data4_io_num = GPIO_NUM_NC,
        .data5_io_num = GPIO_NUM_NC,
        .data6_io_num = GPIO_NUM_NC,
        .data7_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    s_trans_sem = xSemaphoreCreateBinary();

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = GPIO_NUM_NC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .on_color_trans_done = on_color_trans_done,
        .flags = {.quad_mode = true},
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &s_io));

    const co5300_vendor_config_t vendor_cfg = {
        .init_cmds = vendor_init,
        .init_cmds_size = sizeof(vendor_init) / sizeof(vendor_init[0]),
        .flags = {.use_qspi_interface = 1},
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0x06, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    s_flush_lock = xSemaphoreCreateMutex();
    s_frame_lock = xSemaphoreCreateMutex();
    /* 预分配 DMA 安全的行缓冲（32 行 × 932 字节 = 30KB 内部 RAM） */
    s_dma_buf = heap_caps_malloc(LCD_H_RES * 8 * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!s_dma_buf) {
        ESP_LOGE(TAG, "DMA buf alloc failed");
        return ESP_ERR_NO_MEM;
    }
    /* 行缓冲放内部 RAM（DMA 可达），避免 PSRAM 帧缓冲 + 大块 priv TX 分配失败 */
    s_fb = heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_fb) {
        ESP_LOGE(TAG, "framebuffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0, LCD_H_RES * LCD_V_RES * sizeof(uint16_t));
    display_brightness(0xC0);
    ESP_LOGI(TAG, "display ready (%dx%d)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

uint16_t *display_fb(void) { return s_fb; }

/* 帧缓冲互斥：录音电平刷新与整屏切换并发时避免撕裂 */
bool display_frame_take(int wait_ms)
{
    return s_frame_lock && xSemaphoreTake(s_frame_lock, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
}

void display_frame_give(void)
{
    if (s_frame_lock) xSemaphoreGive(s_frame_lock);
}

i2c_master_bus_handle_t display_i2c_bus(void) { return s_i2c_bus; }

void display_fill(uint16_t color)
{
    if (!s_fb) return;
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) s_fb[i] = color;
}

bool display_flush_rows(int y0, int y1)
{
    if (!s_panel || !s_fb || !s_dma_buf || y0 < 0 || y1 > LCD_V_RES || y0 >= y1)
        return false;
    if (xSemaphoreTake(s_flush_lock, 0) != pdTRUE) return false;
    bool ok = true;
    if (s_dma_pending) {
        if (xSemaphoreTake(s_trans_sem, 0) != pdTRUE) {
            xSemaphoreGive(s_flush_lock);
            return false;
        }
        s_dma_pending = false;
    }
    for (int y = y0; y < y1; y += 8) {
        int end = y + 8 < y1 ? y + 8 : y1;
        memcpy(s_dma_buf, s_fb + (size_t)y * LCD_H_RES,
               (end - y) * LCD_H_RES * sizeof(uint16_t));
        esp_err_t rc = esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_H_RES, end, s_dma_buf);
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "display submit failed y=%d rc=%s", y, esp_err_to_name(rc));
            ok = false;
            break;
        }
        s_dma_pending = true;
        if (xSemaphoreTake(s_trans_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "display DMA timeout y=%d", y);
            ok = false;
            break;
        }
        s_dma_pending = false;
    }
    if (ok) s_drawn_frames++;
    else s_failed_frames++;
    xSemaphoreGive(s_flush_lock);
    return ok;
}

uint32_t display_full_generation(void) { return __atomic_load_n(&s_full_generation, __ATOMIC_RELAXED); }

void display_log_diagnostics(void)
{
    ESP_LOGI(TAG, "health frames=%lu failed=%lu dma_pending=%d",
             (unsigned long)s_drawn_frames, (unsigned long)s_failed_frames, s_dma_pending);
}

void display_flush(void)
{
    __atomic_add_fetch(&s_full_generation, 1, __ATOMIC_RELAXED);
    display_flush_rows(0, LCD_V_RES);
}
