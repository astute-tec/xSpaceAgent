idf_component_register(
    SRCS "main.c" "display.c" "qrcodegen.c" "mic.c" "recorder.c" "touch.c" "queue.c" "afe.c" "menu.c" "font_cn.c" "blufi_prov.c"
    EMBED_FILES "web/index.html"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi nvs_flash esp_http_server esp_lcd esp_driver_spi esp_driver_i2c esp_driver_gpio esp_driver_usb_serial_jtag esp_netif lwip esp_http_client esp_driver_i2s esp_timer esp_lcd_touch mbedtls spi_flash fatfs wear_levelling bt esp_wifi esp_ringbuf
)
