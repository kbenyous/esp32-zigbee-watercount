/*
 * Pilotage de la LED RGB d'état.
 *
 * Le module ne connaît aucun état applicatif : il affiche un motif de fond
 * (éteint, fixe ou clignotant) sur lequel peut se superposer un flash bref.
 * C'est la logique applicative qui choisit les couleurs.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_color_t;

#define LED_COLOR_OFF    ((led_color_t){0, 0, 0})
#define LED_COLOR_RED    ((led_color_t){255, 0, 0})
#define LED_COLOR_ORANGE ((led_color_t){255, 25, 0})
#define LED_COLOR_GREEN  ((led_color_t){0, 255, 0})
#define LED_COLOR_BLUE   ((led_color_t){0, 0, 255})

/** rgb_order : LED attendant l'ordre RGB au lieu du GRB standard du WS2812. */
esp_err_t led_init(int gpio, uint8_t max_brightness, bool rgb_order);

/** Motif de fond : couleur fixe (LED_COLOR_OFF pour éteindre). */
void led_set_steady(led_color_t color);

/** Motif de fond : clignotement à 50 % de rapport cyclique. */
void led_set_blinking(led_color_t color, uint32_t period_ms);

/** Flash unique superposé au motif de fond, qui reprend ensuite. */
void led_flash(led_color_t color, uint32_t duration_ms);
