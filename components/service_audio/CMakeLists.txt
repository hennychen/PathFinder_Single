idf_component_register(
    SRCS "service_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES app_state app_status drivers_audio
)
