#include "power_fail.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_check.h"

static const char *TAG = "power_fail";

static power_fail_isr_cb_t s_cb;
static void *s_cb_ctx;

static void power_fail_isr(void *arg)
{
    /* Un seul signalement suffit : l'alimentation ne reviendra pas avant le prochain boot. */
    gpio_intr_disable((int)(intptr_t)arg);
    s_cb(s_cb_ctx);
}

esp_err_t power_fail_start(int gpio, bool active_low, power_fail_isr_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .intr_type = active_low ? GPIO_INTR_NEGEDGE : GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config");

    return gpio_isr_handler_add(gpio, power_fail_isr, (void *)(intptr_t)gpio);
}
