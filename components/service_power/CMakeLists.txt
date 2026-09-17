idf_component_register(
    SRCS "service_power.c"
    INCLUDE_DIRS "include"
    REQUIRES app_state app_status board_support drivers_power
)
