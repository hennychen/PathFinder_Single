idf_component_register(
    SRCS "service_ui.c" "ui_pages.c" "ui_render.c" "fonts/nav_road_font.c" "fonts/nav_num_font.c"
    INCLUDE_DIRS "include"
    REQUIRES app_pages app_state app_status board_support drivers_display service_attitude service_voice
)
