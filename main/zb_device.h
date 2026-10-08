/*
 * Device Zigbee : end device exposant un compteur d'eau (cluster Metering).
 *
 * Modèle ZCL de l'endpoint :
 *  - Basic (serveur)    : identité du device ;
 *  - Identify (serveur) : requis par le profil HA ;
 *  - Metering (serveur) : CurrentSummationDelivered = nombre d'impulsions,
 *    Multiplier / Divisor / UnitOfMeasure tels que
 *    volume = impulsions x Multiplier / Divisor, dans UnitOfMeasure.
 *    Ces attributs standard sont en lecture seule, comme le veut la spec.
 *
 * Toute la configuration passe par des attributs manufacturer-specific du
 * cluster Metering (code fabricant de zb_device_config_t), invisibles pour
 * un client qui ne précise pas ce code :
 *  - ZB_DEVICE_ATTR_UNIT         (enum8)  : 0 = litres, 1 = mètres cubes ;
 *  - ZB_DEVICE_ATTR_PULSE_VOLUME (uint32) : volume d'une impulsion en µL ;
 *  - ZB_DEVICE_ATTR_COUNTER      (uint48) : compteur d'impulsions, écrire pour le régler.
 *
 * Le module ne prend aucune décision applicative : il signale les évènements
 * réseau et les écritures d'attributs via un callback, appelé depuis la tâche
 * Zigbee. Les fonctions publiques peuvent être appelées depuis n'importe
 * quelle tâche (elles prennent le verrou de la stack), mais pas depuis le callback.
 * Avant ZB_DEVICE_EVT_STARTED, la stack peut ne pas être prête : les mises à jour
 * d'attributs retournent alors ESP_ERR_INVALID_STATE et sont à refaire ensuite.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define ZB_DEVICE_ATTR_UNIT         0xF000
#define ZB_DEVICE_ATTR_PULSE_VOLUME 0xF001
#define ZB_DEVICE_ATTR_COUNTER      0xF002

#define ZB_DEVICE_COUNTER_MAX 0xFFFFFFFFFFFFULL /*!< Plage d'un uint48 */

typedef enum {
    ZB_DEVICE_UNIT_LITRES = 0,
    ZB_DEVICE_UNIT_CUBIC_METERS = 1,
} zb_device_unit_t;

/** Paramètres fixes du device, fournis au démarrage. */
typedef struct {
    const char *manufacturer_name; /*!< Basic ManufacturerName, 32 caractères max */
    const char *model_identifier;  /*!< Basic ModelIdentifier, 32 caractères max */
    uint8_t hw_version;            /*!< Basic HWVersion */
    uint8_t app_version;           /*!< Basic ApplicationVersion */
    uint16_t manufacturer_code;    /*!< Node descriptor et attributs de configuration */
    uint8_t endpoint;
    int16_t min_join_lqi;          /*!< Seuil LQI de join, -1 pour garder celui de la lib */
} zb_device_config_t;

typedef struct {
    zb_device_unit_t unit;
    uint32_t pulse_volume_ul; /*!< Volume d'une impulsion en microlitres, > 0 */
} zb_device_metering_config_t;

typedef enum {
    ZB_DEVICE_EVT_STARTED,         /*!< Stack initialisée ; joined indique si le device est sur un réseau */
    ZB_DEVICE_EVT_JOINED,          /*!< Network steering réussi */
    ZB_DEVICE_EVT_JOIN_FAILED,     /*!< Network steering terminé sans réseau */
    ZB_DEVICE_EVT_LEFT,            /*!< Device sorti du réseau (reset local ou demandé par le coordinateur) */
    ZB_DEVICE_EVT_CONNECTION_LOST, /*!< Réseau connu mais injoignable ; le module retente seul */
    ZB_DEVICE_EVT_UNIT_WRITTEN,
    ZB_DEVICE_EVT_PULSE_VOLUME_WRITTEN,
    ZB_DEVICE_EVT_COUNTER_WRITTEN,
} zb_device_event_type_t;

typedef struct {
    zb_device_event_type_t type;
    union {
        bool joined;              /*!< ZB_DEVICE_EVT_STARTED */
        zb_device_unit_t unit;    /*!< ZB_DEVICE_EVT_UNIT_WRITTEN */
        uint32_t pulse_volume_ul; /*!< ZB_DEVICE_EVT_PULSE_VOLUME_WRITTEN */
        uint64_t counter;         /*!< ZB_DEVICE_EVT_COUNTER_WRITTEN */
    };
} zb_device_event_t;

typedef void (*zb_device_event_cb_t)(const zb_device_event_t *event, void *ctx);

/**
 * Démarre la stack dans sa propre tâche, avec les valeurs initiales des attributs.
 * La partition NVS par défaut doit déjà être initialisée (nvs_flash_init()).
 */
esp_err_t zb_device_start(const zb_device_config_t *config, uint64_t counter,
                          const zb_device_metering_config_t *metering, zb_device_event_cb_t cb, void *ctx);

/** Vrai si le device possède des informations de réseau (appairé, joignable ou non). */
bool zb_device_has_network(void);

/** Lance une recherche de réseau ouvert ; résultat via ZB_DEVICE_EVT_JOINED / JOIN_FAILED. */
esp_err_t zb_device_start_steering(void);

/** Quitte le réseau et efface ses informations ; fin signalée par ZB_DEVICE_EVT_LEFT. */
void zb_device_leave_network(void);

/** Efface les datasets Zigbee et redémarre la puce. */
__attribute__((noreturn)) void zb_device_factory_reset(void);

esp_err_t zb_device_set_counter(uint64_t counter);
esp_err_t zb_device_set_metering_config(const zb_device_metering_config_t *config);
