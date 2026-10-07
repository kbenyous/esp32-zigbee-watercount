/*
 * Entrée de comptage des impulsions.
 *
 * L'anti-rebond est fait dans une tâche dédiée (contrainte temps réel) : le
 * GPIO déclenche une interruption sur chaque front, la tâche attend que le
 * niveau soit stable puis compte la transition vers le niveau actif.
 *
 * Les impulsions détectées s'accumulent dans un compteur interne que la
 * logique applicative vide avec pulse_input_take(). Le callback n'est qu'une
 * notification : si l'application en manque une, aucune impulsion n'est perdue.
 *
 * Prérequis : le service ISR GPIO est installé (gpio_install_isr_service()).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    PULSE_INPUT_PULL_NONE,
    PULSE_INPUT_PULL_UP,
    PULSE_INPUT_PULL_DOWN,
} pulse_input_pull_t;

typedef struct {
    int gpio;
    bool active_high;
    pulse_input_pull_t pull;
    uint32_t debounce_ms;
} pulse_input_config_t;

/** Appelé depuis la tâche de comptage à chaque impulsion validée. */
typedef void (*pulse_input_cb_t)(void *ctx);

esp_err_t pulse_input_start(const pulse_input_config_t *config, pulse_input_cb_t cb, void *ctx);

/** Retourne le nombre d'impulsions détectées depuis l'appel précédent. */
uint32_t pulse_input_take(void);
