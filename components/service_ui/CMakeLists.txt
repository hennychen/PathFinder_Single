idf_component_register(
    SRCS "service_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES app_pages app_state app_status board_support drivers_display service_voice
)
