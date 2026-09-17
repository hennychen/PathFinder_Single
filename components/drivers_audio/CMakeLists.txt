idf_component_register(
    SRCS "drivers_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES app_status board_support driver esp_driver_i2s
)
