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

# Convertisseur Zigbee2MQTT aux valeurs de menuconfig (identité, code fabricant, endpoint) :
# build/z2m/watercounter.js, régénéré quand sdkconfig ou le modèle change.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    function(wc_js_string var value)
        string(REPLACE "\\" "\\\\" value "${value}")
        string(REPLACE "'" "\\'" value "${value}")
        set(${var} "'${value}'" PARENT_SCOPE)
    endfunction()
    wc_js_string(Z2M_MANUFACTURER_NAME "${CONFIG_WC_ZB_MANUFACTURER_NAME}")
    wc_js_string(Z2M_MODEL_IDENTIFIER "${CONFIG_WC_ZB_MODEL_IDENTIFIER}")
    configure_file(${CMAKE_CURRENT_LIST_DIR}/../z2m/watercounter.js.in ${CMAKE_BINARY_DIR}/z2m/watercounter.js @ONLY)
endif()
