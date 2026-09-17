set(main_requires
    app_status
    board_support
    drivers_pcf85063
    drivers_qmi8658
)

if(NOT (DEFINED PATHFINDER_QMI_MIN_BRINGUP AND PATHFINDER_QMI_MIN_BRINGUP))
    list(APPEND main_requires
        app_pages
        app_state
        service_audio
        service_power
        service_log
        service_ui
        service_attitude
        service_voice
        service_ble
        service_navigation
        service_obd
    )
endif()

idf_component_register(
    SRCS "app_main.c"
    REQUIRES ${main_requires}
)

if(DEFINED PATHFINDER_QMI_MIN_BRINGUP AND PATHFINDER_QMI_MIN_BRINGUP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PATHFINDER_QMI_MIN_BRINGUP=1)
endif()
