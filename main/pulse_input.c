#include "pulse_input.h"

#include <stdatomic.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pulse";

static pulse_input_config_t s_config;
static pulse_input_cb_t s_cb;
static void *s_cb_ctx;
static TaskHandle_t s_task;
static atomic_uint_fast32_t s_pending;

static void pulse_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    /* Masqué jusqu'à la fin de l'anti-rebond : les rebonds ne réveillent pas la tâche. */
    gpio_intr_disable(s_config.gpio);
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void pulse_task(void *arg)
{
    const int active_level = s_config.active_high ? 1 : 0;
    int stable_level = gpio_get_level(s_config.gpio);

    gpio_intr_enable(s_config.gpio);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool again;
        do {
            vTaskDelay(pdMS_TO_TICKS(s_config.debounce_ms));
            int level = gpio_get_level(s_config.gpio);
            if (level != stable_level) {
                stable_level = level;
                if (level == active_level) {
                    atomic_fetch_add(&s_pending, 1);
                    s_cb(s_cb_ctx);
                }
            }
            gpio_intr_enable(s_config.gpio);
            /* Un front survenu entre la lecture et la réactivation de l'interruption
             * n'aurait pas déclenché l'ISR : on le rattrape ici. */
            again = gpio_get_level(s_config.gpio) != stable_level;
            if (again) {
                gpio_intr_disable(s_config.gpio);
            }
        } while (again);
    }
}

esp_err_t pulse_input_start(const pulse_input_config_t *config, pulse_input_cb_t cb, void *ctx)
{
    s_config = *config;
    s_cb = cb;
    s_cb_ctx = ctx;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << config->gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = config->pull == PULSE_INPUT_PULL_UP,
        .pull_down_en = config->pull == PULSE_INPUT_PULL_DOWN,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config");
    gpio_intr_disable(config->gpio);

    /* L'interruption reste masquée jusqu'à ce que la tâche la démasque. */
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(config->gpio, pulse_isr, NULL), TAG, "isr handler");
    /* Priorité haute : la tâche ne fait qu'attendre et lire un GPIO. */
    ESP_RETURN_ON_FALSE(xTaskCreate(pulse_task, "pulse", 2560, NULL, configMAX_PRIORITIES - 2, &s_task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");

    ESP_LOGI(TAG, "Pulse input on GPIO%d, active %s, debounce %lums", config->gpio,
             config->active_high ? "high" : "low", (unsigned long)config->debounce_ms);
    return ESP_OK;
}

uint32_t pulse_input_take(void)
{
    return atomic_exchange(&s_pending, 0);
}
