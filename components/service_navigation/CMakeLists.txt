idf_component_register(
    SRCS "navigation_protocol.c" "service_navigation.c"
    INCLUDE_DIRS "include"
    REQUIRES app_pages app_state app_status service_ble service_obd
)
