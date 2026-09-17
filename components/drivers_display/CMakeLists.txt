idf_component_register(
    SRCS "drivers_display.c"
    INCLUDE_DIRS "include"
    REQUIRES app_status board_support driver esp_driver_gpio esp_driver_spi esp_lcd esp_lcd_spd2010 esp_lcd_touch esp_lcd_touch_spd2010 esp_lvgl_port
)
