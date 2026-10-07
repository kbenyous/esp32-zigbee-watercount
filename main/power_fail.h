/*
 * Détection optionnelle de coupure d'alimentation imminente.
 *
 * Nécessite du matériel : une entrée qui bascule dès que l'alimentation amont
 * disparaît, et assez de réserve d'énergie (condensateur) pour que l'ESP
 * termine une écriture NVS. Sans ce matériel, le module n'est pas démarré.
 *
 * Prérequis : le service ISR GPIO est installé (gpio_install_isr_service()).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** Appelé DEPUIS L'ISR : ne doit utiliser que des API *FromISR. */
typedef void (*power_fail_isr_cb_t)(void *ctx);

esp_err_t power_fail_start(int gpio, bool active_low, power_fail_isr_cb_t cb, void *ctx);
