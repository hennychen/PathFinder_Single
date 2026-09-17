idf_component_register(
    SRCS "service_attitude.c"
    INCLUDE_DIRS "include"
    REQUIRES app_state app_status drivers_qmi8658 esp_timer
)
