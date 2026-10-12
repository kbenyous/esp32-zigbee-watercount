idf_component_register(SRCS "main.c"
                            "app.c"
                            "button.c"
                            "led.c"
                            "power_fail.c"
                            "pulse_input.c"
                            "storage.c"
                            "zb_device.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_gpio esp_timer esp_app_format esp_system nvs_flash)

if(CONFIG_WC_ZB_PREFER_BEST_PARENT)
    # Fait choisir le parent au meilleur LQI (cf. zb_device.c).
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=nwk_join_request")
endif()

if(CONFIG_WC_ZB_DEBUG_LOG)
    # Affiche la table des parents découverts avant que la stack ne la vide (cf. zb_device.c).
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=nwk_disc_table_reset")
endif()
