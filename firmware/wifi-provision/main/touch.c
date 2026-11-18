/* CST9217 触摸：点屏幕开始/结束录音 */
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst9217.h"

#include "board.h"
#include "display.h"
#include "touch.h"

static const char *TAG = "touch";
static esp_lcd_touch_handle_t s_tp;

esp_err_t touch_init(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    io_cfg.scl_speed_hz = 400 * 1000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus(), &io_cfg, &io));

    esp_lcd_touch_config_t cfg = {
        .x_max = LCD_H_RES - 1,
        .y_max = LCD_V_RES - 1,
        .rst_gpio_num = PIN_TOUCH_RST,
        .int_gpio_num = PIN_TOUCH_INT,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 1, .mirror_y = 1},
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst9217(io, &cfg, &s_tp));
    ESP_LOGI(TAG, "touch ready");
    return ESP_OK;
}

int touch_press_xy(uint16_t *x, uint16_t *y)
{
    static bool was_down;
    if (!s_tp) return 0;
    esp_lcd_touch_read_data(s_tp);
    uint16_t xs[1], ys[1], strength[1];
    uint8_t cnt = 0;
    bool down = esp_lcd_touch_get_coordinates(s_tp, xs, ys, strength, &cnt, 1);
    bool now = down && cnt > 0;
    int edge = (now && !was_down) ? 1 : 0;
    was_down = now;
    if (edge && x && y) { *x = xs[0]; *y = ys[0]; }
    return edge;
}

int touch_poll_xy(uint16_t *x, uint16_t *y, int *down)
{
    if (!s_tp) { if (down) *down = 0; return 0; }
    esp_lcd_touch_read_data(s_tp);
    uint16_t xs[1], ys[1], strength[1];
    uint8_t cnt = 0;
    bool now = esp_lcd_touch_get_coordinates(s_tp, xs, ys, strength, &cnt, 1) && cnt > 0;
    if (x && y && now) { *x = xs[0]; *y = ys[0]; }
    if (down) *down = now ? 1 : 0;
    return 1;
}

int touch_took_press(void)
{
    static bool was_down;
    if (!s_tp) return 0;
    esp_lcd_touch_read_data(s_tp);
    uint16_t x[1], y[1], strength[1];
    uint8_t cnt = 0;
    bool down = esp_lcd_touch_get_coordinates(s_tp, x, y, strength, &cnt, 1);
    bool now = down && cnt > 0;
    int edge = (now && !was_down) ? 1 : 0;
    was_down = now;
    return edge;
}
