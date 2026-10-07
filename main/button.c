#include "button.h"

#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BUTTON_DEBOUNCE_MS 30

static const char *TAG = "button";

static int s_gpio;
static uint32_t s_long_press_ms;
static button_cb_t s_cb;
static void *s_cb_ctx;
static TaskHandle_t s_task;

static void button_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static bool is_pressed(void)
{
    return gpio_get_level(s_gpio) == 0;
}

/* Attend un niveau stable égal à pressed, au plus timeout. */
static bool wait_stable(bool pressed, TickType_t timeout)
{
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        if (is_pressed() == pressed) {
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            if (is_pressed() == pressed) {
                return true;
            }
        }
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (timeout != portMAX_DELAY && elapsed >= timeout) {
            return false;
        }
        ulTaskNotifyTake(pdTRUE, timeout == portMAX_DELAY ? portMAX_DELAY : timeout - elapsed);
    }
}

static void button_task(void *arg)
{
    for (;;) {
        wait_stable(true, portMAX_DELAY);
        if (wait_stable(false, pdMS_TO_TICKS(s_long_press_ms))) {
            s_cb(BUTTON_EVENT_SHORT_PRESS, s_cb_ctx);
        } else {
            s_cb(BUTTON_EVENT_LONG_PRESS, s_cb_ctx);
            wait_stable(false, portMAX_DELAY);
        }
    }
}

esp_err_t button_start(int gpio, uint32_t long_press_ms, button_cb_t cb, void *ctx)
{
    s_gpio = gpio;
    s_long_press_ms = long_press_ms;
    s_cb = cb;
    s_cb_ctx = ctx;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio_config");

    ESP_RETURN_ON_FALSE(xTaskCreate(button_task, "button", 2560, NULL, 5, &s_task) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "task");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(gpio, button_isr, NULL), TAG, "isr handler");
    return ESP_OK;
}
