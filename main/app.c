#include "app.h"

#include <inttypes.h>
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "button.h"
#include "led.h"
#include "power_fail.h"
#include "pulse_input.h"
#include "storage.h"
#include "zb_device.h"

#define APP_QUEUE_LEN            16
#define LED_PAIRING_PERIOD_MS    500
#define LED_PULSE_FLASH_MS       80
/* Délai laissé à la stack pour quitter proprement le réseau avant un reset forcé. */
#define LEAVE_TIMEOUT_MS         5000
#define PAIR_ON_BOOT_MAGIC       0x50414952U /* "PAIR" */

/* Traduction des choix Kconfig (un symbole bool désactivé n'est pas défini). */
#if CONFIG_WC_DEFAULT_UNIT_LITRES
#define DEFAULT_UNIT ZB_DEVICE_UNIT_LITRES
#else
#define DEFAULT_UNIT ZB_DEVICE_UNIT_CUBIC_METERS
#endif

#if CONFIG_WC_PULSE_PULL_UP
#define PULSE_PULL PULSE_INPUT_PULL_UP
#elif CONFIG_WC_PULSE_PULL_DOWN
#define PULSE_PULL PULSE_INPUT_PULL_DOWN
#else
#define PULSE_PULL PULSE_INPUT_PULL_NONE
#endif

#if CONFIG_WC_PULSE_ACTIVE_HIGH
#define PULSE_ACTIVE_HIGH true
#else
#define PULSE_ACTIVE_HIGH false
#endif

#if CONFIG_WC_POWER_FAIL_ACTIVE_LOW
#define POWER_FAIL_ACTIVE_LOW true
#else
#define POWER_FAIL_ACTIVE_LOW false
#endif

static const char *TAG = "app";

typedef enum {
    APP_EVT_PULSE,
    APP_EVT_BUTTON,
    APP_EVT_ZIGBEE,
    APP_EVT_PERSIST_TIMEOUT,
    APP_EVT_PAIRING_RETRY,
    APP_EVT_PAIRING_TIMEOUT,
    APP_EVT_LEAVE_TIMEOUT,
    APP_EVT_POWER_FAIL,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    union {
        button_event_t button;
        zb_device_event_t zigbee;
    };
} app_event_t;

typedef enum {
    NET_STARTING,     /*!< Stack en cours d'initialisation */
    NET_NOT_PAIRED,   /*!< Aucun réseau : attente d'un appui long */
    NET_PAIRING,      /*!< Recherche d'un réseau ouvert */
    NET_JOINED,       /*!< Sur le réseau, fonctionnel */
    NET_DISCONNECTED, /*!< Réseau connu mais injoignable, la stack retente */
} net_state_t;

static QueueHandle_t s_queue;

/* Le compteur est aussi lu et modifié par le shutdown handler, qui s'exécute
 * dans la tâche qui appelle esp_restart() : accès sous section critique. */
static portMUX_TYPE s_counter_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_counter;
static uint64_t s_persisted_counter;
static uint32_t s_pulses_since_persist;

static storage_config_t s_config;
static net_state_t s_net_state = NET_STARTING;
static bool s_leaving_for_pairing;

static esp_timer_handle_t s_persist_timer;
static esp_timer_handle_t s_pairing_retry_timer;
static esp_timer_handle_t s_pairing_timeout_timer;
static esp_timer_handle_t s_leave_timer;

/* Demande d'appairage survivant au redémarrage provoqué par un reset Zigbee forcé. */
static RTC_NOINIT_ATTR uint32_t s_pair_on_boot;

/* ------------------------------------------------------------------------- */
/* Callbacks des modules : postent dans la file, rien d'autre                */
/* ------------------------------------------------------------------------- */

static void post(const app_event_t *event)
{
    if (xQueueSend(s_queue, event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full, event %d dropped", event->type);
    }
}

static void on_pulse(void *ctx)
{
    /* File pleine : sans conséquence, les impulsions restent comptées dans
     * pulse_input et seront prises au prochain évènement. */
    app_event_t event = {.type = APP_EVT_PULSE};
    xQueueSend(s_queue, &event, 0);
}

static void on_button(button_event_t button, void *ctx)
{
    app_event_t event = {.type = APP_EVT_BUTTON, .button = button};
    post(&event);
}

static void on_zigbee(const zb_device_event_t *zigbee, void *ctx)
{
    app_event_t event = {.type = APP_EVT_ZIGBEE, .zigbee = *zigbee};
    post(&event);
}

#if CONFIG_WC_POWER_FAIL_GPIO >= 0
static void on_power_fail_isr(void *ctx)
{
    BaseType_t woken = pdFALSE;
    app_event_t event = {.type = APP_EVT_POWER_FAIL};
    xQueueSendToFrontFromISR(s_queue, &event, &woken);
    portYIELD_FROM_ISR(woken);
}
#endif

static void on_timer(void *arg)
{
    app_event_t event = {.type = (app_event_type_t)(intptr_t)arg};
    post(&event);
}

/* ------------------------------------------------------------------------- */
/* Compteur et persistance                                                   */
/* ------------------------------------------------------------------------- */

static uint64_t counter_get(void)
{
    portENTER_CRITICAL(&s_counter_lock);
    uint64_t counter = s_counter;
    portEXIT_CRITICAL(&s_counter_lock);
    return counter;
}

static void counter_set(uint64_t counter)
{
    portENTER_CRITICAL(&s_counter_lock);
    s_counter = counter;
    storage_shadow_write(counter);
    portEXIT_CRITICAL(&s_counter_lock);
}

static uint64_t counter_add(uint32_t pulses)
{
    portENTER_CRITICAL(&s_counter_lock);
    s_counter = (s_counter + pulses) & ZB_DEVICE_COUNTER_MAX;
    uint64_t counter = s_counter;
    storage_shadow_write(counter);
    portEXIT_CRITICAL(&s_counter_lock);
    return counter;
}

static void persist_counter(void)
{
    esp_timer_stop(s_persist_timer);
    s_pulses_since_persist = 0;
    uint64_t counter = counter_get();
    if (counter != s_persisted_counter && storage_save_counter(counter) == ESP_OK) {
        s_persisted_counter = counter;
    }
}

/* Écriture immédiate, impulsions pas encore traitées par la boucle comprises. */
static void persist_now(void)
{
    counter_add(pulse_input_take());
    persist_counter();
}

static void handle_pulses(void)
{
    uint32_t pulses = pulse_input_take();
    if (pulses == 0) {
        return;
    }
    uint64_t counter = counter_add(pulses);
    ESP_LOGI(TAG, "+%" PRIu32 " pulse(s), total %" PRIu64, pulses, counter);
    led_flash(LED_COLOR_BLUE, LED_PULSE_FLASH_MS);
    zb_device_set_counter(counter);

    s_pulses_since_persist += pulses;
    if (s_pulses_since_persist >= CONFIG_WC_PERSIST_EVERY_N_PULSES) {
        persist_counter();
    } else {
        esp_timer_stop(s_persist_timer);
        esp_timer_start_once(s_persist_timer, (uint64_t)CONFIG_WC_PERSIST_IDLE_TIMEOUT_S * 1000 * 1000);
    }
}

static void load_counter(void)
{
    uint64_t persisted = 0;
    storage_load_counter(&persisted);
    s_persisted_counter = persisted;

    uint64_t shadow;
    if (storage_shadow_read(&shadow)) {
        if (shadow != persisted) {
            ESP_LOGW(TAG, "Recovered counter %" PRIu64 " from RTC memory (NVS had %" PRIu64 ")", shadow, persisted);
        }
        counter_set(shadow);
        persist_counter();
    } else {
        counter_set(persisted);
    }
    ESP_LOGI(TAG, "Counter: %" PRIu64 " pulses (reset reason %d)", counter_get(), esp_reset_reason());
}

/* ------------------------------------------------------------------------- */
/* Configuration                                                             */
/* ------------------------------------------------------------------------- */

static zb_device_metering_config_t metering_config(void)
{
    return (zb_device_metering_config_t){
        .unit = s_config.unit,
        .pulse_volume_ul = s_config.pulse_volume_ul,
    };
}

static void load_config(void)
{
    if (!storage_load_config(&s_config)) {
        s_config.unit = DEFAULT_UNIT;
        s_config.pulse_volume_ul = CONFIG_WC_DEFAULT_PULSE_VOLUME_UL;
    }
}

static void apply_config(void)
{
    if (storage_save_config(&s_config) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot persist configuration");
    }
    zb_device_metering_config_t metering = metering_config();
    zb_device_set_metering_config(&metering);
}

/* ------------------------------------------------------------------------- */
/* Réseau, appairage et LED                                                  */
/* ------------------------------------------------------------------------- */

static void set_net_state(net_state_t state)
{
    s_net_state = state;
    switch (state) {
    case NET_STARTING:
        led_set_steady(LED_COLOR_OFF);
        break;
    case NET_NOT_PAIRED:
    case NET_DISCONNECTED:
        led_set_steady(LED_COLOR_RED);
        break;
    case NET_PAIRING:
        led_set_blinking(LED_COLOR_ORANGE, LED_PAIRING_PERIOD_MS);
        break;
    case NET_JOINED:
        led_set_steady(LED_COLOR_GREEN);
        break;
    }
}

static void stop_pairing_timers(void)
{
    esp_timer_stop(s_pairing_retry_timer);
    esp_timer_stop(s_pairing_timeout_timer);
    esp_timer_stop(s_leave_timer);
}

static void start_steering(void)
{
    esp_err_t err = zb_device_start_steering();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot start network steering (%s), retrying", esp_err_to_name(err));
        esp_timer_start_once(s_pairing_retry_timer, (uint64_t)CONFIG_WC_PAIRING_RETRY_S * 1000 * 1000);
    }
}

static void start_pairing(void)
{
    ESP_LOGI(TAG, "Pairing started");
    stop_pairing_timers();
    set_net_state(NET_PAIRING);
    esp_timer_start_once(s_pairing_timeout_timer, (uint64_t)CONFIG_WC_PAIRING_TIMEOUT_S * 1000 * 1000);

    if (zb_device_has_network()) {
        /* Déjà appairé : on quitte d'abord le réseau, le steering suivra l'évènement LEFT. */
        s_leaving_for_pairing = true;
        esp_timer_start_once(s_leave_timer, (uint64_t)LEAVE_TIMEOUT_MS * 1000);
        zb_device_leave_network();
    } else {
        start_steering();
    }
}

static void handle_long_press(void)
{
    switch (s_net_state) {
    case NET_STARTING:
        ESP_LOGW(TAG, "Zigbee stack not ready, long press ignored");
        break;
    case NET_PAIRING:
        if (!s_leaving_for_pairing) {
            ESP_LOGI(TAG, "Pairing already in progress, timeout extended");
            esp_timer_stop(s_pairing_timeout_timer);
            esp_timer_start_once(s_pairing_timeout_timer, (uint64_t)CONFIG_WC_PAIRING_TIMEOUT_S * 1000 * 1000);
        }
        break;
    default:
        start_pairing();
        break;
    }
}

static void handle_zigbee(const zb_device_event_t *event)
{
    switch (event->type) {
    case ZB_DEVICE_EVT_STARTED:
        /* Impulsions comptées pendant l'initialisation de la stack. */
        zb_device_set_counter(counter_get());
        if (event->joined) {
            set_net_state(NET_JOINED);
        } else if (s_pair_on_boot == PAIR_ON_BOOT_MAGIC) {
            start_pairing();
        } else {
            set_net_state(NET_NOT_PAIRED);
        }
        s_pair_on_boot = 0;
        break;

    case ZB_DEVICE_EVT_JOINED:
        ESP_LOGI(TAG, "Paired");
        stop_pairing_timers();
        set_net_state(NET_JOINED);
        break;

    case ZB_DEVICE_EVT_JOIN_FAILED:
        if (s_net_state == NET_PAIRING) {
            esp_timer_start_once(s_pairing_retry_timer, (uint64_t)CONFIG_WC_PAIRING_RETRY_S * 1000 * 1000);
        }
        break;

    case ZB_DEVICE_EVT_LEFT:
        esp_timer_stop(s_leave_timer);
        if (s_leaving_for_pairing) {
            s_leaving_for_pairing = false;
            start_steering();
        } else {
            ESP_LOGI(TAG, "Removed from network");
            stop_pairing_timers();
            set_net_state(NET_NOT_PAIRED);
        }
        break;

    case ZB_DEVICE_EVT_CONNECTION_LOST:
        if (s_net_state != NET_PAIRING) {
            set_net_state(NET_DISCONNECTED);
        }
        break;

    case ZB_DEVICE_EVT_UNIT_WRITTEN:
        ESP_LOGI(TAG, "Unit set to %s", event->unit == ZB_DEVICE_UNIT_LITRES ? "litres" : "cubic meters");
        s_config.unit = event->unit;
        apply_config();
        break;

    case ZB_DEVICE_EVT_PULSE_VOLUME_WRITTEN:
        ESP_LOGI(TAG, "Pulse volume set to %" PRIu32 " uL", event->pulse_volume_ul);
        s_config.pulse_volume_ul = event->pulse_volume_ul;
        apply_config();
        break;

    case ZB_DEVICE_EVT_COUNTER_WRITTEN:
        ESP_LOGI(TAG, "Counter set to %" PRIu64, event->counter);
        /* Les impulsions arrivées entre-temps appartiennent à l'ancienne valeur. */
        pulse_input_take();
        counter_set(event->counter);
        persist_counter();
        zb_device_set_counter(event->counter);
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Boucle d'évènements                                                       */
/* ------------------------------------------------------------------------- */

static void app_task(void *arg)
{
    app_event_t event;
    for (;;) {
        xQueueReceive(s_queue, &event, portMAX_DELAY);
        switch (event.type) {
        case APP_EVT_PULSE:
            handle_pulses();
            break;
        case APP_EVT_BUTTON:
            if (event.button == BUTTON_EVENT_LONG_PRESS) {
                handle_long_press();
            }
            break;
        case APP_EVT_ZIGBEE:
            handle_zigbee(&event.zigbee);
            break;
        case APP_EVT_PERSIST_TIMEOUT:
            persist_counter();
            break;
        case APP_EVT_PAIRING_RETRY:
            if (s_net_state == NET_PAIRING && !s_leaving_for_pairing) {
                start_steering();
            }
            break;
        case APP_EVT_PAIRING_TIMEOUT:
            if (s_net_state == NET_PAIRING) {
                ESP_LOGW(TAG, "Pairing timed out");
                stop_pairing_timers();
                s_leaving_for_pairing = false;
                set_net_state(NET_NOT_PAIRED);
            }
            break;
        case APP_EVT_LEAVE_TIMEOUT:
            if (s_leaving_for_pairing) {
                /* La sortie propre du réseau n'a pas abouti : reset Zigbee complet,
                 * l'appairage reprendra après le redémarrage. */
                ESP_LOGW(TAG, "Network leave timed out, forcing Zigbee factory reset");
                s_pair_on_boot = PAIR_ON_BOOT_MAGIC;
                zb_device_factory_reset();
            }
            break;
        case APP_EVT_POWER_FAIL:
            ESP_LOGW(TAG, "Power failure detected");
            persist_now();
            break;
        }
    }
}

static esp_err_t create_timer(app_event_type_t type, const char *name, esp_timer_handle_t *timer)
{
    const esp_timer_create_args_t args = {
        .callback = on_timer,
        .arg = (void *)(intptr_t)type,
        .name = name,
    };
    return esp_timer_create(&args, timer);
}

esp_err_t app_start(void)
{
    s_queue = xQueueCreate(APP_QUEUE_LEN, sizeof(app_event_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "queue");
    ESP_RETURN_ON_ERROR(create_timer(APP_EVT_PERSIST_TIMEOUT, "persist", &s_persist_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(create_timer(APP_EVT_PAIRING_RETRY, "pair_retry", &s_pairing_retry_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(create_timer(APP_EVT_PAIRING_TIMEOUT, "pair_timeout", &s_pairing_timeout_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(create_timer(APP_EVT_LEAVE_TIMEOUT, "leave_timeout", &s_leave_timer), TAG, "timer");

    ESP_RETURN_ON_ERROR(storage_init(), TAG, "storage");
    load_counter();
    load_config();
    /* esp_restart() (reset Zigbee, futur OTA...) appelle ce handler avant de redémarrer. */
    ESP_RETURN_ON_ERROR(esp_register_shutdown_handler(persist_now), TAG, "shutdown handler");

    ESP_RETURN_ON_ERROR(led_init(CONFIG_WC_LED_GPIO, CONFIG_WC_LED_BRIGHTNESS), TAG, "led");
    set_net_state(NET_STARTING);

    const zb_device_config_t zb_config = {
        .manufacturer_name = CONFIG_WC_ZB_MANUFACTURER_NAME,
        .model_identifier = CONFIG_WC_ZB_MODEL_IDENTIFIER,
        .hw_version = CONFIG_WC_ZB_HW_VERSION,
        .app_version = CONFIG_WC_ZB_APP_VERSION,
        .manufacturer_code = CONFIG_WC_ZB_MANUFACTURER_CODE,
        .endpoint = CONFIG_WC_ZB_ENDPOINT,
        .min_join_lqi = CONFIG_WC_ZB_MIN_JOIN_LQI,
    };
    const zb_device_metering_config_t metering = metering_config();
    ESP_RETURN_ON_ERROR(zb_device_start(&zb_config, counter_get(), &metering, on_zigbee, NULL), TAG, "zigbee");

    /* Service ISR GPIO partagé par pulse_input, button et power_fail. */
    ESP_RETURN_ON_ERROR(gpio_install_isr_service(0), TAG, "gpio isr service");

    const pulse_input_config_t pulse_config = {
        .gpio = CONFIG_WC_PULSE_GPIO,
        .active_high = PULSE_ACTIVE_HIGH,
        .pull = PULSE_PULL,
        .debounce_ms = CONFIG_WC_PULSE_DEBOUNCE_MS,
    };
    ESP_RETURN_ON_ERROR(pulse_input_start(&pulse_config, on_pulse, NULL), TAG, "pulse input");
    ESP_RETURN_ON_ERROR(button_start(CONFIG_WC_BUTTON_GPIO, CONFIG_WC_BUTTON_LONG_PRESS_MS, on_button, NULL), TAG,
                        "button");
#if CONFIG_WC_POWER_FAIL_GPIO >= 0
    ESP_RETURN_ON_ERROR(power_fail_start(CONFIG_WC_POWER_FAIL_GPIO, POWER_FAIL_ACTIVE_LOW,
                                         on_power_fail_isr, NULL),
                        TAG, "power fail");
#endif

    ESP_RETURN_ON_FALSE(xTaskCreate(app_task, "app", 4096, NULL, 4, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}
