idf_component_register(
    SRCS "drivers_power.c"
    INCLUDE_DIRS "include"
    REQUIRES app_status board_support esp_adc
)
