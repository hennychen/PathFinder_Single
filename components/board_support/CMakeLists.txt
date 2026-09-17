idf_component_register(
    SRCS "board_support.c"
    INCLUDE_DIRS "include"
    REQUIRES app_status drivers_tca9554 esp_driver_gpio esp_driver_i2c
)
