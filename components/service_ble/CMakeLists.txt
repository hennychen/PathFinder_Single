idf_component_register(
    SRCS "service_ble.c" "service_ble_client.c"
    INCLUDE_DIRS "include"
    REQUIRES app_state app_status bt nvs_flash
)
