idf_component_register(
    SRCS "obd_adapter_profile.c" "obd_protocol.c" "obd_provider_esp_obd_ii.c" "obd_provider_service_ble_elm327.c" "service_obd.c"
    INCLUDE_DIRS "include"
    REQUIRES app_state app_status esp_obd_ii service_ble
)
