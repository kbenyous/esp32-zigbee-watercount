#include "storage.h"

#include <inttypes.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NAMESPACE       "watercount"
#define KEY_COUNTER         "counter"
#define KEY_UNIT            "unit"
#define KEY_PULSE_VOLUME_UL "pulse_ul"

#define SHADOW_MAGIC 0x57434E54U /* "WCNT" */

static const char *TAG = "storage";

static nvs_handle_t s_nvs;

typedef struct {
    uint32_t magic;
    uint32_t check;
    uint64_t counter;
} counter_shadow_t;

static RTC_NOINIT_ATTR counter_shadow_t s_shadow;

static uint32_t shadow_check(uint64_t counter)
{
    return ~((uint32_t)counter ^ (uint32_t)(counter >> 32) ^ SHADOW_MAGIC);
}

esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition unusable (%s), erasing", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init");
    return nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
}

/* Lecture NVS commune : trace le résultat, retourne true si la clé existe. */
static bool read_ok(esp_err_t err, const char *key)
{
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "NVS read: '%s' not stored yet", key);
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS read: '%s' failed (%s)", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool storage_load_counter(uint64_t *counter)
{
    if (!read_ok(nvs_get_u64(s_nvs, KEY_COUNTER, counter), KEY_COUNTER)) {
        return false;
    }
    ESP_LOGI(TAG, "NVS read: counter = %" PRIu64, *counter);
    return true;
}

esp_err_t storage_save_counter(uint64_t counter)
{
    /* nvs_set_* n'écrit rien en flash si la valeur stockée est identique. */
    ESP_RETURN_ON_ERROR(nvs_set_u64(s_nvs, KEY_COUNTER, counter), TAG, "set counter");
    ESP_RETURN_ON_ERROR(nvs_commit(s_nvs), TAG, "commit");
    ESP_LOGI(TAG, "NVS write: counter = %" PRIu64, counter);
    return ESP_OK;
}

bool storage_load_config(storage_config_t *config)
{
    storage_config_t c;
    if (!read_ok(nvs_get_u8(s_nvs, KEY_UNIT, &c.unit), KEY_UNIT) ||
        !read_ok(nvs_get_u32(s_nvs, KEY_PULSE_VOLUME_UL, &c.pulse_volume_ul), KEY_PULSE_VOLUME_UL)) {
        return false;
    }
    ESP_LOGI(TAG, "NVS read: unit = %u, pulse volume = %" PRIu32 " uL", c.unit, c.pulse_volume_ul);
    *config = c;
    return true;
}

esp_err_t storage_save_config(const storage_config_t *config)
{
    ESP_RETURN_ON_ERROR(nvs_set_u8(s_nvs, KEY_UNIT, config->unit), TAG, "set unit");
    ESP_RETURN_ON_ERROR(nvs_set_u32(s_nvs, KEY_PULSE_VOLUME_UL, config->pulse_volume_ul), TAG, "set pulse volume");
    ESP_RETURN_ON_ERROR(nvs_commit(s_nvs), TAG, "commit");
    ESP_LOGI(TAG, "NVS write: unit = %u, pulse volume = %" PRIu32 " uL", config->unit, config->pulse_volume_ul);
    return ESP_OK;
}

void storage_shadow_write(uint64_t counter)
{
    /* Un reset au milieu de la mise à jour laisse un check incohérent :
     * la shadow est alors ignorée au boot et la NVS fait foi. */
    s_shadow.counter = counter;
    s_shadow.check = shadow_check(counter);
    s_shadow.magic = SHADOW_MAGIC;
}

bool storage_shadow_read(uint64_t *counter)
{
    if (s_shadow.magic != SHADOW_MAGIC || s_shadow.check != shadow_check(s_shadow.counter)) {
        return false;
    }
    *counter = s_shadow.counter;
    return true;
}
