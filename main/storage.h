/*
 * Persistance du compteur et de la configuration.
 *
 * Deux niveaux :
 *  - la NVS (flash), écrite seulement quand la logique applicative le décide ;
 *  - une copie "shadow" en mémoire RTC non initialisée, mise à jour à chaque
 *    impulsion sans aucune usure de flash. Elle survit aux resets logiciels,
 *    panics, watchdogs et brownouts tant que l'alimentation n'est pas coupée ;
 *    pas au bouton RST (CHIP_PU), qui réinitialise toute la puce.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t unit;             /*!< Unité d'affichage, cf. zb_device_unit_t */
    uint32_t pulse_volume_ul; /*!< Volume par impulsion, en microlitres */
} storage_config_t;

esp_err_t storage_init(void);

/** Retourne false si aucune valeur n'a encore été enregistrée. */
bool storage_load_counter(uint64_t *counter);
esp_err_t storage_save_counter(uint64_t counter);

/** Retourne false si aucune configuration n'a encore été enregistrée. */
bool storage_load_config(storage_config_t *config);
esp_err_t storage_save_config(const storage_config_t *config);

void storage_shadow_write(uint64_t counter);
/** Retourne false si la shadow est invalide (mise sous tension, corruption). */
bool storage_shadow_read(uint64_t *counter);
