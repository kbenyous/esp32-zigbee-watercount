/*
 * Bouton poussoir avec détection d'appui long.
 *
 * Le bouton est actif à l'état bas (pull-up interne). Une tâche dédiée,
 * réveillée par interruption, fait l'anti-rebond et mesure la durée d'appui.
 *
 * Prérequis : le service ISR GPIO est installé (gpio_install_isr_service()).
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    BUTTON_EVENT_SHORT_PRESS, /*!< Relâché avant la durée d'appui long */
    BUTTON_EVENT_LONG_PRESS,  /*!< Maintenu pendant la durée d'appui long (émis sans attendre le relâchement) */
} button_event_t;

/** Appelé depuis la tâche du bouton. */
typedef void (*button_cb_t)(button_event_t event, void *ctx);

esp_err_t button_start(int gpio, uint32_t long_press_ms, button_cb_t cb, void *ctx);
