#include "led.h"

#include <stdbool.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"

static const char *TAG = "led";

static led_strip_handle_t s_strip;
static SemaphoreHandle_t s_mutex;
static esp_timer_handle_t s_blink_timer;
static esp_timer_handle_t s_flash_timer;
static uint8_t s_max_brightness;

/* État protégé par s_mutex */
static led_color_t s_background;
static bool s_blinking;
static bool s_blink_phase_on;
static bool s_flash_active;
static led_color_t s_flash_color;

static void render_locked(void)
{
    led_color_t c;
    if (s_flash_active) {
        c = s_flash_color;
    } else if (s_blinking && !s_blink_phase_on) {
        c = LED_COLOR_OFF;
    } else {
        c = s_background;
    }
    led_strip_set_pixel(s_strip, 0,
                        c.r * s_max_brightness / 255,
                        c.g * s_max_brightness / 255,
                        c.b * s_max_brightness / 255);
    led_strip_refresh(s_strip);
}

static void blink_timer_cb(void *arg)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_blink_phase_on = !s_blink_phase_on;
    render_locked();
    xSemaphoreGive(s_mutex);
}

static void flash_timer_cb(void *arg)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_flash_active = false;
    render_locked();
    xSemaphoreGive(s_mutex);
}

esp_err_t led_init(int gpio, uint8_t max_brightness)
{
    s_max_brightness = max_brightness;
    s_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mutex, ESP_ERR_NO_MEM, TAG, "mutex");

    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip), TAG, "led_strip");

    const esp_timer_create_args_t blink_args = {.callback = blink_timer_cb, .name = "led_blink"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&blink_args, &s_blink_timer), TAG, "blink timer");
    const esp_timer_create_args_t flash_args = {.callback = flash_timer_cb, .name = "led_flash"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&flash_args, &s_flash_timer), TAG, "flash timer");

    led_set_steady(LED_COLOR_OFF);
    return ESP_OK;
}

void led_set_steady(led_color_t color)
{
    esp_timer_stop(s_blink_timer);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_background = color;
    s_blinking = false;
    render_locked();
    xSemaphoreGive(s_mutex);
}

void led_set_blinking(led_color_t color, uint32_t period_ms)
{
    esp_timer_stop(s_blink_timer);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_background = color;
    s_blinking = true;
    s_blink_phase_on = true;
    render_locked();
    xSemaphoreGive(s_mutex);
    esp_timer_start_periodic(s_blink_timer, (uint64_t)period_ms * 1000 / 2);
}

void led_flash(led_color_t color, uint32_t duration_ms)
{
    esp_timer_stop(s_flash_timer);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_flash_active = true;
    s_flash_color = color;
    render_locked();
    xSemaphoreGive(s_mutex);
    esp_timer_start_once(s_flash_timer, (uint64_t)duration_ms * 1000);
}
