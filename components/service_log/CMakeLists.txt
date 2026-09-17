idf_component_register(
    SRCS "service_log.c"
    INCLUDE_DIRS "include"
    REQUIRES app_status board_support driver esp_driver_gpio esp_driver_spi fatfs sdmmc
)
