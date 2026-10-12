#include "sdkconfig.h"
#if CONFIG_WC_ZB_DEBUG_LOG
/* ESP_LOGD actifs dans ce fichier sans relever CONFIG_LOG_MAXIMUM_LEVEL pour tout le projet. */
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#endif

#include "zb_device.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_zigbee.h"
#include "ezbee/platform/log.h"
#include "ezbee/platform/radio.h"
#include "ezbee/zha.h"

/* La stack range ses datasets dans le namespace "zigbee" de cette partition.
 * Elle ne l'initialise pas elle-même : nvs_flash_init() doit avoir été appelé. */
#define ZB_STORAGE_PARTITION "nvs"

/* End device non-sleepy : la radio reste à l'écoute, le device répond à tout
 * instant. Le keep-alive vers le parent reprend la valeur des exemples officiels. */
#define ZB_ED_TIMEOUT     EZB_NWK_ED_TIMEOUT_64MIN
#define ZB_KEEP_ALIVE_MS  4000

#define ZB_REJOIN_RETRY_MIN_MS (10 * 1000)
#define ZB_REJOIN_RETRY_MAX_MS (5 * 60 * 1000)

#define ZCL_UINT24_MAX 0xFFFFFFU
#define ZCL_STRING_MAX 32

static const char *TAG = "zb_device";

static zb_device_config_t s_config;
static zb_device_event_cb_t s_cb;
static void *s_cb_ctx;

/* Valeurs initiales des attributs. Statiques : la stack peut conserver ces pointeurs. */
static uint64_t s_initial_counter;
static zb_device_metering_config_t s_initial_metering;
static uint8_t s_initial_unit;

static esp_timer_handle_t s_commissioning_timer;
static ezb_bdb_comm_mode_mask_t s_commissioning_timer_mode;
static uint32_t s_rejoin_retry_ms = ZB_REJOIN_RETRY_MIN_MS;

/* Vrai une fois la stack initialisée et le modèle de données enregistré :
 * avant, le verrou de la stack n'existe pas encore. */
static volatile bool s_ready;

/* Vrai pendant que le module modifie lui-même un attribut, pour ne pas
 * prendre cette mise à jour pour une écriture venue du réseau. */
static bool s_local_update;

/* Chaînes ZCL : premier octet = longueur. Statiques car référencées par la stack. */
static char s_zcl_manufacturer_name[1 + ZCL_STRING_MAX];
static char s_zcl_model_identifier[1 + ZCL_STRING_MAX];
static char s_zcl_date_code[1 + 16];
static char s_zcl_sw_build_id[1 + 16];
static char s_zcl_serial_number[1 + 16];

/* ------------------------------------------------------------------------- */
/* Représentation ZCL de la configuration du compteur                        */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t unit_of_measure;
    uint32_t multiplier;
    uint32_t divisor;
    uint8_t summation_formatting;
} metering_format_t;

static uint64_t gcd_u64(uint64_t a, uint64_t b)
{
    while (b) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* Meilleure approximation de num/den par une fraction dont les deux termes
 * tiennent dans max (réduites de la fraction continue). */
static void bounded_fraction(uint64_t num, uint64_t den, uint32_t max, uint32_t *out_num, uint32_t *out_den)
{
    uint64_t g = gcd_u64(num, den);
    num /= g;
    den /= g;
    if (num <= max && den <= max) {
        *out_num = num;
        *out_den = den;
        return;
    }
    uint64_t p0 = 0, q0 = 1, p1 = 1, q1 = 0;
    while (den != 0) {
        uint64_t a = num / den;
        uint64_t p2 = a * p1 + p0;
        uint64_t q2 = a * q1 + q0;
        if (p2 > max || q2 > max) {
            break;
        }
        p0 = p1, q0 = q1, p1 = p2, q1 = q2;
        uint64_t r = num - a * den;
        num = den;
        den = r;
    }
    if (p1 == 0) {
        /* Valeur plus petite que 1/max : on garde la plus petite valeur non nulle. */
        p1 = 1;
        q1 = max;
    }
    *out_num = p1;
    *out_den = q1;
}

static void metering_format(const zb_device_metering_config_t *config, metering_format_t *out)
{
    const bool litres = config->unit == ZB_DEVICE_UNIT_LITRES;
    const uint64_t ul_per_unit = litres ? 1000000ULL : 1000000000ULL;

    out->unit_of_measure = litres ? EZB_ZCL_METERING_UNIT_L_LH_BINARY : EZB_ZCL_METERING_UNIT_M3_M3H_BINARY;
    bounded_fraction(config->pulse_volume_ul, ul_per_unit, ZCL_UINT24_MAX, &out->multiplier, &out->divisor);

    /* SummationFormatting : bits 0-2 chiffres après la virgule, bits 3-6 chiffres
     * avant, bit 7 suppression des zéros de tête. Simple indication d'affichage. */
    uint8_t decimals = 0;
    for (uint64_t p = 1; p < out->divisor && decimals < 7; p *= 10) {
        decimals++;
    }
    out->summation_formatting = 0x80 | (9 << 3) | decimals;
}

/* ------------------------------------------------------------------------- */
/* Construction du modèle de données                                         */
/* ------------------------------------------------------------------------- */

static void zcl_string(char *dst, size_t max_len, const char *src)
{
    size_t len = strnlen(src, max_len);
    dst[0] = (char)len;
    memcpy(dst + 1, src, len);
}

static void build_identity_strings(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    zcl_string(s_zcl_manufacturer_name, ZCL_STRING_MAX, s_config.manufacturer_name);
    zcl_string(s_zcl_model_identifier, ZCL_STRING_MAX, s_config.model_identifier);
    zcl_string(s_zcl_sw_build_id, 16, app->version);

    /* DateCode au format ZCL AAAAMMJJ, depuis la date de compilation "Mmm dd yyyy". */
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char month_str[4] = {0};
    int day = 0, year = 0;
    sscanf(app->date, "%3s %d %d", month_str, &day, &year);
    const char *m = strstr(months, month_str);
    int month = m ? (int)(m - months) / 3 + 1 : 0;
    char date[40];
    snprintf(date, sizeof(date), "%04d%02d%02d", year, month, day);
    zcl_string(s_zcl_date_code, 16, date);

    /* Numéro de série : l'EUI-64 IEEE 802.15.4, unique par puce. */
    uint8_t eui[8];
    char serial[17];
    esp_read_mac(eui, ESP_MAC_IEEE802154);
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X%02X%02X", eui[0], eui[1], eui[2], eui[3], eui[4],
             eui[5], eui[6], eui[7]);
    zcl_string(s_zcl_serial_number, 16, serial);
}

static void add_attr_checked(ezb_err_t err, const char *what)
{
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Cannot add attribute %s (err %d)", what, err);
    }
}

static ezb_zcl_cluster_desc_t create_basic_cluster(void)
{
    ezb_zcl_basic_cluster_server_config_t cfg = {
        .zcl_version = EZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = EZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE,
    };
    ezb_zcl_cluster_desc_t basic = ezb_zcl_basic_create_cluster_desc(&cfg, EZB_ZCL_CLUSTER_SERVER);

    add_attr_checked(ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                                         s_zcl_manufacturer_name), "ManufacturerName");
    add_attr_checked(ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                                         s_zcl_model_identifier), "ModelIdentifier");
    add_attr_checked(ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_DATE_CODE_ID, s_zcl_date_code),
                     "DateCode");
    add_attr_checked(ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_SW_BUILD_ID_ID, s_zcl_sw_build_id),
                     "SWBuildID");
    add_attr_checked(ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_SERIAL_NUMBER_ID,
                                                         s_zcl_serial_number), "SerialNumber");
    add_attr_checked(ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID,
                                                         &s_config.app_version), "ApplicationVersion");
    add_attr_checked(ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_HW_VERSION_ID,
                                                         &s_config.hw_version), "HWVersion");
    return basic;
}

static ezb_zcl_cluster_desc_t create_metering_cluster(void)
{
    static metering_format_t fmt;
    metering_format(&s_initial_metering, &fmt);
    s_initial_unit = s_initial_metering.unit;

    ezb_zcl_metering_cluster_server_config_t cfg = {
        .current_summation_delivered = s_initial_counter,
        .status = EZB_ZCL_METERING_STATUS_DEFAULT_VALUE,
        .unit_of_measure = fmt.unit_of_measure,
        .summation_formatting = fmt.summation_formatting,
        .metering_device_type = EZB_ZCL_METERING_WATER_METERING,
    };
    ezb_zcl_cluster_desc_t metering = ezb_zcl_metering_create_cluster_desc(&cfg, EZB_ZCL_CLUSTER_SERVER);

    add_attr_checked(ezb_zcl_metering_cluster_desc_add_attr(metering, EZB_ZCL_ATTR_METERING_MULTIPLIER_ID,
                                                            &fmt.multiplier), "Multiplier");
    add_attr_checked(ezb_zcl_metering_cluster_desc_add_attr(metering, EZB_ZCL_ATTR_METERING_DIVISOR_ID,
                                                            &fmt.divisor), "Divisor");

    /* CurrentSummationDelivered est reportable selon la spec ZCL : on le déclare
     * explicitement plutôt que de dépendre de l'accès posé par défaut par la lib. */
    ezb_zcl_attr_desc_t summation = ezb_zcl_cluster_get_attr_desc(
        metering, EZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID, EZB_ZCL_STD_MANUF_CODE);
    ezb_zcl_attr_desc_set_access(summation, ezb_zcl_attr_desc_get_access(summation) | EZB_ZCL_ATTR_ACCESS_REPORTING);

    /* Configuration, manufacturer-specific */
    add_attr_checked(ezb_zcl_cluster_desc_add_manuf_attr(metering, ZB_DEVICE_ATTR_UNIT, EZB_ZCL_ATTR_TYPE_ENUM8,
                                                         EZB_ZCL_ATTR_ACCESS_READ_WRITE, s_config.manufacturer_code,
                                                         &s_initial_unit), "unit");
    add_attr_checked(ezb_zcl_cluster_desc_add_manuf_attr(metering, ZB_DEVICE_ATTR_PULSE_VOLUME,
                                                         EZB_ZCL_ATTR_TYPE_UINT32, EZB_ZCL_ATTR_ACCESS_READ_WRITE,
                                                         s_config.manufacturer_code,
                                                         &s_initial_metering.pulse_volume_ul), "pulse volume");
    add_attr_checked(ezb_zcl_cluster_desc_add_manuf_attr(metering, ZB_DEVICE_ATTR_COUNTER, EZB_ZCL_ATTR_TYPE_UINT48,
                                                         EZB_ZCL_ATTR_ACCESS_READ_WRITE, s_config.manufacturer_code,
                                                         &s_initial_counter), "counter");
    return metering;
}

static void create_device(void)
{
    ezb_af_ep_config_t ep_cfg = {
        .ep_id = s_config.endpoint,
        .app_profile_id = EZB_AF_HA_PROFILE_ID,
        .app_device_id = EZB_ZHA_METER_INTERFACE_DEVICE_ID,
        .app_device_version = 0,
    };
    ezb_af_ep_desc_t ep = ezb_af_create_endpoint_desc(&ep_cfg);

    ezb_zcl_identify_cluster_server_config_t identify_cfg = {
        .identify_time = EZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, create_basic_cluster()));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(
        ep, ezb_zcl_identify_create_cluster_desc(&identify_cfg, EZB_ZCL_CLUSTER_SERVER)));
    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep, create_metering_cluster()));

    ezb_af_device_desc_t dev = ezb_af_create_device_desc();
    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev, ep));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev));
}

/* ------------------------------------------------------------------------- */
/* Évènements de la stack (contexte de la tâche Zigbee)                      */
/* ------------------------------------------------------------------------- */

static void emit(const zb_device_event_t *event)
{
    s_cb(event, s_cb_ctx);
}

static void emit_type(zb_device_event_type_t type)
{
    zb_device_event_t event = {.type = type};
    emit(&event);
}

static void commissioning_timer_cb(void *arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_bdb_start_top_level_commissioning(s_commissioning_timer_mode);
    esp_zigbee_lock_release();
}

static void schedule_commissioning(ezb_bdb_comm_mode_mask_t mode, uint32_t delay_ms)
{
    esp_timer_stop(s_commissioning_timer);
    s_commissioning_timer_mode = mode;
    esp_timer_start_once(s_commissioning_timer, (uint64_t)delay_ms * 1000);
}

static void log_network(void)
{
    ezb_extpanid_t ext_pan_id;
    ezb_nwk_get_extended_panid(&ext_pan_id);
    ESP_LOGI(TAG, "On network PAN 0x%04hx (ext 0x%016llx), channel %d, short address 0x%04hx", ezb_nwk_get_panid(),
             ext_pan_id.u64, ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
}

/* Qualité des liens vers les voisins, à comparer au seuil de join (niveau debug). */
static void log_neighbors(void)
{
    int8_t tx_power;
    ezb_nwk_get_tx_power(&tx_power);
    ESP_LOGD(TAG, "TX power: %d dBm, min join LQI: %u", tx_power, ezb_nwk_get_min_join_lqi());
    ezb_nwk_info_iterator_t it = EZB_NWK_INFO_ITERATOR_INIT;
    ezb_nwk_neighbor_info_t nbr;
    unsigned count = 0;
    while (ezb_nwk_get_next_neighbor(&it, &nbr) == EZB_ERR_NONE) {
        count++;
        ESP_LOGD(TAG, "  neighbor 0x%04hx: type %u, LQI %u, RSSI %d dBm", nbr.short_addr, nbr.device_type, nbr.lqi,
                 nbr.rssi);
    }
    ESP_LOGD(TAG, "%u neighbor(s) in table", count);
}

static bool app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t type = ezb_app_signal_get_type(app_signal);

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        const ezb_bdb_signal_simple_params_t *params = ezb_app_signal_get_params(app_signal);
        if (params->status == EZB_BDB_STATUS_SUCCESS) {
            s_rejoin_retry_ms = ZB_REJOIN_RETRY_MIN_MS;
            zb_device_event_t event = {.type = ZB_DEVICE_EVT_STARTED, .joined = !ezb_bdb_is_factory_new()};
            if (event.joined) {
                log_network();
                log_neighbors();
            }
            emit(&event);
        } else if (type == EZB_BDB_SIGNAL_DEVICE_REBOOT) {
            /* Réseau connu mais injoignable (coordinateur éteint, hors de portée...). */
            ezb_extpanid_t ext_pan_id;
            ezb_nwk_get_extended_panid(&ext_pan_id);
            ESP_LOGW(TAG, "Rejoin of PAN 0x%04hx (ext 0x%016llx) on channel %d failed (status %d), retry in %lus",
                     ezb_nwk_get_panid(), ext_pan_id.u64, ezb_nwk_get_current_channel(), params->status,
                     (unsigned long)(s_rejoin_retry_ms / 1000));
            emit_type(ZB_DEVICE_EVT_CONNECTION_LOST);
            schedule_commissioning(EZB_BDB_MODE_INITIALIZATION, s_rejoin_retry_ms);
            s_rejoin_retry_ms = MIN(s_rejoin_retry_ms * 2, ZB_REJOIN_RETRY_MAX_MS);
        } else {
            ESP_LOGW(TAG, "Stack initialization failed (status %d), retrying", params->status);
            schedule_commissioning(EZB_BDB_MODE_INITIALIZATION, 1000);
        }
    } break;

    case EZB_BDB_SIGNAL_STEERING: {
        const ezb_bdb_signal_simple_params_t *params = ezb_app_signal_get_params(app_signal);
        if (params->status == EZB_BDB_STATUS_SUCCESS) {
            log_network();
            log_neighbors();
            emit_type(ZB_DEVICE_EVT_JOINED);
        } else {
            ESP_LOGI(TAG, "Network steering finished without network (status %d)", params->status);
            emit_type(ZB_DEVICE_EVT_JOIN_FAILED);
        }
    } break;

    case EZB_ZDO_SIGNAL_LEAVE: {
        const ezb_zdo_signal_leave_params_t *params = ezb_app_signal_get_params(app_signal);
        if (params->leave_type == EZB_ZDO_LEAVE_TYPE_RESET) {
            ESP_LOGI(TAG, "Left network");
            emit_type(ZB_DEVICE_EVT_LEFT);
        } else {
            ESP_LOGI(TAG, "Left network, rejoining");
        }
    } break;

    default:
        ESP_LOGD(TAG, "Signal %s (0x%04x)", ezb_app_signal_to_string(type), type);
        break;
    }
    return true;
}

static void handle_set_attr_value(ezb_zcl_set_attr_value_message_t *msg)
{
    msg->out.result = EZB_ZCL_STATUS_SUCCESS;
    if (s_local_update || msg->info.dst_ep != s_config.endpoint ||
        msg->info.cluster_id != EZB_ZCL_CLUSTER_ID_METERING) {
        return;
    }

    const ezb_zcl_attribute_t *attr = &msg->in.attribute;
    zb_device_event_t event;
    switch (attr->id) {
    case ZB_DEVICE_ATTR_UNIT: {
        uint8_t unit = *(const uint8_t *)attr->data.value;
        if (unit != ZB_DEVICE_UNIT_LITRES && unit != ZB_DEVICE_UNIT_CUBIC_METERS) {
            msg->out.result = EZB_ZCL_STATUS_INVALID_VALUE;
            return;
        }
        event = (zb_device_event_t){.type = ZB_DEVICE_EVT_UNIT_WRITTEN, .unit = unit};
    } break;
    case ZB_DEVICE_ATTR_PULSE_VOLUME: {
        uint32_t volume;
        memcpy(&volume, attr->data.value, sizeof(volume));
        if (volume == 0) {
            msg->out.result = EZB_ZCL_STATUS_INVALID_VALUE;
            return;
        }
        event = (zb_device_event_t){.type = ZB_DEVICE_EVT_PULSE_VOLUME_WRITTEN, .pulse_volume_ul = volume};
    } break;
    case ZB_DEVICE_ATTR_COUNTER: {
        /* uint48 little-endian : on ne lit que 6 octets, quel que soit le stockage interne. */
        uint64_t counter = 0;
        memcpy(&counter, attr->data.value, 6);
        event = (zb_device_event_t){.type = ZB_DEVICE_EVT_COUNTER_WRITTEN, .counter = counter};
    } break;
    default:
        return;
    }
    emit(&event);
}

static void zcl_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID:
        handle_set_attr_value(message);
        break;
    default:
        ESP_LOGD(TAG, "ZCL core action 0x%04lx", (unsigned long)callback_id);
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Tâche Zigbee                                                              */
/* ------------------------------------------------------------------------- */

static void zb_task(void *arg)
{
    esp_zigbee_config_t config = {
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE,
            .install_code_policy = false,
            .zed_config = {
                .ed_timeout = ZB_ED_TIMEOUT,
                .keep_alive = ZB_KEEP_ALIVE_MS,
            },
        },
        .platform_config = {
            .storage_partition_name = ZB_STORAGE_PARTITION,
            .radio_config = {
                .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,
            },
        },
    };
    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    /* Zigbee2MQTT forme un réseau centralisé (Trust Center sur le coordinateur). */
    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(EZB_RADIO_2P4GHZ_ALL_CHANNEL_MASK));
    ezb_nwk_set_rx_on_when_idle(true);
    if (s_config.min_join_lqi >= 0) {
        ezb_nwk_set_min_join_lqi((uint8_t)s_config.min_join_lqi);
    }
    ezb_af_node_desc_set_manuf_code(s_config.manufacturer_code);
    ezb_af_node_power_desc_t power_desc = {
        .current_power_mode = EZB_AF_NODE_POWER_MODE_SYNC_ON_WHEN_IDLE,
        .available_power_sources = EZB_AF_NODE_POWER_SOURCE_CONSTANT_POWER,
        .current_power_source = EZB_AF_NODE_POWER_SOURCE_CONSTANT_POWER,
        .current_power_source_level = EZB_AF_NODE_POWER_SOURCE_LEVEL_100_PERCENT,
    };
    ezb_af_set_node_power_desc(&power_desc);

    ESP_ERROR_CHECK(ezb_app_signal_add_handler(app_signal_handler));
    create_device();
    ezb_zcl_core_action_handler_register(zcl_action_handler);
    s_ready = true;

    ESP_ERROR_CHECK(esp_zigbee_start(false));
    esp_zigbee_launch_mainloop();

    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------------- */
/* API publique                                                              */
/* ------------------------------------------------------------------------- */

#if CONFIG_WC_ZB_DEBUG_LOG
#define ZB_STACK_LOG_TAG "ezbee"

/* Remplace l'implémentation faible d'esp-zigbee-lib, qui n'affiche que les erreurs. */
void ezb_plat_log(ezb_log_level_t log_level, const char *format, ...)
{
    if (log_level <= EZB_LOG_LEVEL_NONE) {
        return;
    }
    char line[160];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    ESP_LOG_LEVEL((esp_log_level_t)log_level, ZB_STACK_LOG_TAG, "%s", line);
}

/* Fonctions internes d'esp-zigbee-lib 2.0.4, absentes des headers publics. */
void nwk_disc_table_dump(void);
void __real_nwk_disc_table_reset(void);

/* La stack vide sa table des parents découverts à la fin de chaque commissioning,
 * avant de prévenir l'application : on l'affiche juste avant (LQI, permit join).
 * Branché par -Wl,--wrap=nwk_disc_table_reset, cf. CMakeLists.txt. */
void __wrap_nwk_disc_table_reset(void)
{
    nwk_disc_table_dump();
    __real_nwk_disc_table_reset();
}
#endif

#if CONFIG_WC_ZB_PREFER_BEST_PARENT
/* Entrée de la table de découverte d'esp-zigbee-lib 2.0.4 (nwk_disc_table.c),
 * reconstituée par désassemblage ; seuls les champs lus ici sont nommés. */
typedef struct {
    uint16_t short_addr; /* adresse du routeur qui a émis le beacon */
    uint8_t reserved0[6];
    uint8_t pan_ref;     /* index du PAN dans la table de correspondance de la stack */
    uint8_t lqa;         /* qualité de lien, comparée au seuil de join */
    uint8_t reserved1[10];
} zb_disc_entry_t;
_Static_assert(sizeof(zb_disc_entry_t) == 20, "esp-zigbee-lib 2.0.4 discovery entry layout");

/* Fonctions internes d'esp-zigbee-lib 2.0.4, absentes des headers publics. */
const void *nwk_disc_table_next(const void *prev);
int __real_nwk_join_request(void *req);

/* Parmi les candidats au-dessus du seuil, la stack retient le dernier de sa table,
 * pas le meilleur. Le choix se fait de façon synchrone dans nwk_join_request : on y
 * relève le seuil au meilleur LQI du PAN visé (celui de la première entrée, comme
 * zdo_comm_join), puis on le rétablit. Si l'association échoue, la stack retente
 * avec le seuil normal, donc avec son choix habituel parmi les entrées restantes.
 * Branché par -Wl,--wrap=nwk_join_request, cf. CMakeLists.txt. */
int __wrap_nwk_join_request(void *req)
{
    const zb_disc_entry_t *first = nwk_disc_table_next(NULL);
    const zb_disc_entry_t *best = NULL;
    for (const zb_disc_entry_t *e = first; e != NULL; e = nwk_disc_table_next(e)) {
        if (e->pan_ref == first->pan_ref && (best == NULL || e->lqa > best->lqa)) {
            best = e;
        }
    }
    uint8_t min_lqi = ezb_nwk_get_min_join_lqi();
    if (best == NULL || best->lqa <= min_lqi) {
        return __real_nwk_join_request(req);
    }
    ESP_LOGI(TAG, "Best parent 0x%04hx, LQI %u", best->short_addr, best->lqa);
    ezb_nwk_set_min_join_lqi(best->lqa);
    int ret = __real_nwk_join_request(req);
    ezb_nwk_set_min_join_lqi(min_lqi);
    return ret;
}
#endif

esp_err_t zb_device_start(const zb_device_config_t *config, uint64_t counter,
                          const zb_device_metering_config_t *metering, zb_device_event_cb_t cb, void *ctx)
{
#if CONFIG_WC_ZB_DEBUG_LOG
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    esp_log_level_set(ZB_STACK_LOG_TAG, ESP_LOG_DEBUG);
#endif
    s_config = *config;
    s_initial_counter = counter;
    s_initial_metering = *metering;
    s_cb = cb;
    s_cb_ctx = ctx;
    build_identity_strings();

    const esp_timer_create_args_t timer_args = {.callback = commissioning_timer_cb, .name = "zb_commission"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_commissioning_timer), TAG, "timer");

    ESP_RETURN_ON_FALSE(xTaskCreate(zb_task, "zigbee", 6144, NULL, 5, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    return ESP_OK;
}

bool zb_device_has_network(void)
{
    if (!s_ready) {
        return false;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    bool has_network = !ezb_bdb_is_factory_new();
    esp_zigbee_lock_release();
    return has_network;
}

esp_err_t zb_device_start_steering(void)
{
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_INVALID_STATE, TAG, "stack not ready");
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_err_t err = ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
    esp_zigbee_lock_release();
    return esp_zigbee_err_to_esp(err);
}

void zb_device_leave_network(void)
{
    if (!s_ready) {
        return;
    }
    esp_timer_stop(s_commissioning_timer);
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_bdb_reset_via_local_action();
    esp_zigbee_lock_release();
}

void zb_device_factory_reset(void)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    esp_zigbee_factory_reset();
}

/* À appeler verrou pris et s_local_update positionné. Retourne 1 en cas d'échec. */
static int set_metering_attr(uint16_t attr_id, uint16_t manuf_code, void *value)
{
    ezb_zcl_status_t status = ezb_zcl_set_attr_value(s_config.endpoint, EZB_ZCL_CLUSTER_ID_METERING,
                                                     EZB_ZCL_CLUSTER_SERVER, attr_id, manuf_code, value, false);
    if (status != EZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "Cannot set metering attribute 0x%04x (status 0x%02x)", attr_id, status);
        return 1;
    }
    return 0;
}

esp_err_t zb_device_set_counter(uint64_t counter)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    s_local_update = true;
    int failures = set_metering_attr(EZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID, EZB_ZCL_STD_MANUF_CODE,
                                     &counter);
    failures += set_metering_attr(ZB_DEVICE_ATTR_COUNTER, s_config.manufacturer_code, &counter);
    s_local_update = false;
    esp_zigbee_lock_release();
    return failures ? ESP_FAIL : ESP_OK;
}

esp_err_t zb_device_set_metering_config(const zb_device_metering_config_t *config)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    metering_format_t fmt;
    metering_format(config, &fmt);
    uint8_t unit = config->unit;
    uint32_t pulse_volume_ul = config->pulse_volume_ul;

    ESP_LOGI(TAG, "Metering: unit %s, %lu uL/pulse -> multiplier %lu, divisor %lu",
             unit == ZB_DEVICE_UNIT_LITRES ? "L" : "m3", (unsigned long)pulse_volume_ul,
             (unsigned long)fmt.multiplier, (unsigned long)fmt.divisor);

    esp_zigbee_lock_acquire(portMAX_DELAY);
    s_local_update = true;
    int failures = 0;
    failures += set_metering_attr(EZB_ZCL_ATTR_METERING_UNIT_OF_MEASURE_ID, EZB_ZCL_STD_MANUF_CODE,
                                  &fmt.unit_of_measure);
    failures += set_metering_attr(EZB_ZCL_ATTR_METERING_MULTIPLIER_ID, EZB_ZCL_STD_MANUF_CODE, &fmt.multiplier);
    failures += set_metering_attr(EZB_ZCL_ATTR_METERING_DIVISOR_ID, EZB_ZCL_STD_MANUF_CODE, &fmt.divisor);
    failures += set_metering_attr(EZB_ZCL_ATTR_METERING_SUMMATION_FORMATTING_ID, EZB_ZCL_STD_MANUF_CODE,
                                  &fmt.summation_formatting);
    failures += set_metering_attr(ZB_DEVICE_ATTR_UNIT, s_config.manufacturer_code, &unit);
    failures += set_metering_attr(ZB_DEVICE_ATTR_PULSE_VOLUME, s_config.manufacturer_code, &pulse_volume_ul);
    s_local_update = false;
    esp_zigbee_lock_release();
    return failures ? ESP_FAIL : ESP_OK;
}
