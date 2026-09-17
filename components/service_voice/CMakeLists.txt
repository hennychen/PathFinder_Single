idf_component_register(
    SRCS "service_voice.c"
    INCLUDE_DIRS "include"
    REQUIRES app_pages app_state app_status drivers_audio service_audio voice_backend
)
