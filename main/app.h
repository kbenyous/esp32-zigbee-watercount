/*
 * Logique applicative du compteur d'eau.
 *
 * Seul endroit où sont prises les décisions : comptage, stratégie de
 * persistance, appairage, état de la LED. Les modules matériels et Zigbee
 * signalent leurs évènements par callback ; ces callbacks ne font que poster
 * dans une file unique, traitée par une seule boucle.
 */
#pragma once

#include "esp_err.h"

/** Initialise les modules et démarre la tâche de la boucle d'évènements. */
esp_err_t app_start(void);
