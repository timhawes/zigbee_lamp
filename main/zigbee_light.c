#include "zigbee_light.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "esp_zigbee.h"
#include "ezbee/zha.h"

#include <inttypes.h>
#include "light_output.h"

static const char *TAG = "zigbee_light";

#define CHANNEL_MASK_ALL      0x07FFF800U /* channels 11-26 */
#define NVS_NAMESPACE         "lamp"
#define SAVE_DEBOUNCE_US      (2000 * 1000)
#define LEAVE_FALLBACK_US     (5000 * 1000)
#define REPORT_COALESCE_US    (300 * 1000)

#define STARTUP_ON_OFF_OFF      0x00
#define STARTUP_ON_OFF_ON       0x01
#define STARTUP_ON_OFF_TOGGLE   0x02
#define STARTUP_ON_OFF_PREVIOUS 0xFF
#define STARTUP_LEVEL_MIN       0x00
#define STARTUP_LEVEL_PREVIOUS  0xFF

#if CONFIG_LAMP_STARTUP_ON
#define STARTUP_ON_OFF_DEFAULT  STARTUP_ON_OFF_ON
#elif CONFIG_LAMP_STARTUP_PREVIOUS
#define STARTUP_ON_OFF_DEFAULT  STARTUP_ON_OFF_PREVIOUS
#else
#define STARTUP_ON_OFF_DEFAULT  STARTUP_ON_OFF_OFF
#endif

/* StartUp* are stored only when a coordinator writes them, so the Kconfig default applies otherwise.
 * Older firmware saved them on every state change under the legacy keys; those are erased on load. */
#define NVS_KEY_SU_ONOFF        "su_onoff_w"
#define NVS_KEY_SU_LEVEL        "su_level_w"

typedef struct {
    bool on;
    uint8_t level;
    uint8_t startup_on_off;
    uint8_t startup_level;
} lamp_state_t;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static lamp_state_t s_state = {
    .on = true,
    .level = LAMP_LEVEL_MAX,
    .startup_on_off = STARTUP_ON_OFF_DEFAULT,
    .startup_level = STARTUP_LEVEL_PREVIOUS,
};
static esp_timer_handle_t s_save_timer;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_leave_fallback_timer;
static esp_timer_handle_t s_report_timer;

/* Device that last sent us a unicast command (the Hue Bridge, normally); local changes are reported
 * to it directly, because the Hue Bridge never binds or configures reporting on third-party lights. */
static struct {
    bool valid;
    uint16_t short_addr;
    uint8_t ep;
} s_controller;
static uint8_t s_retry_mode;
static volatile bool s_joined;

/* ------------------------------------------------------------------------- */
/* Persistence                                                                */

static void state_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved state, using defaults");
        return;
    }
    uint8_t v;
    if (nvs_get_u8(h, "on", &v) == ESP_OK) s_state.on = v;
    if (nvs_get_u8(h, "level", &v) == ESP_OK) s_state.level = v;
    if (nvs_get_u8(h, NVS_KEY_SU_ONOFF, &v) == ESP_OK) s_state.startup_on_off = v;
    if (nvs_get_u8(h, NVS_KEY_SU_LEVEL, &v) == ESP_OK) s_state.startup_level = v;
    if ((nvs_erase_key(h, "su_onoff") == ESP_OK) | (nvs_erase_key(h, "su_level") == ESP_OK)) {
        nvs_commit(h);
        ESP_LOGI(TAG, "Dropped legacy StartUp* keys");
    }
    uint16_t addr;
    if (nvs_get_u16(h, "ctl_addr", &addr) == ESP_OK && nvs_get_u8(h, "ctl_ep", &v) == ESP_OK) {
        s_controller.valid = true;
        s_controller.short_addr = addr;
        s_controller.ep = v;
    }
    nvs_close(h);
}

static void state_save_now(void *arg)
{
    lamp_state_t st;
    portENTER_CRITICAL(&s_state_lock);
    st = s_state;
    portEXIT_CRITICAL(&s_state_lock);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for saving");
        return;
    }
    nvs_set_u8(h, "on", st.on);
    nvs_set_u8(h, "level", st.level);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGD(TAG, "State saved");
}

static void state_save_later(void)
{
    if (s_save_timer) {
        esp_timer_stop(s_save_timer);
        esp_timer_start_once(s_save_timer, SAVE_DEBOUNCE_US);
    }
}

static void controller_remember(uint16_t short_addr, uint8_t ep)
{
    if (s_controller.valid && s_controller.short_addr == short_addr && s_controller.ep == ep) {
        return;
    }
    s_controller.valid = true;
    s_controller.short_addr = short_addr;
    s_controller.ep = ep;
    ESP_LOGI(TAG, "Reporting local changes to 0x%04x ep %d", short_addr, ep);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, "ctl_addr", short_addr);
        nvs_set_u8(h, "ctl_ep", ep);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Persist a StartUp* attribute written by a coordinator. */
static void startup_attr_save(const char *key, uint8_t value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, value);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "%s set to 0x%02x", key, value);
}

static void state_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Apply StartUpOnOff / StartUpCurrentLevel to the restored state. */
static void state_apply_startup(void)
{
    switch (s_state.startup_on_off) {
    case STARTUP_ON_OFF_OFF:    s_state.on = false; break;
    case STARTUP_ON_OFF_ON:     s_state.on = true; break;
    case STARTUP_ON_OFF_TOGGLE: s_state.on = !s_state.on; break;
    default: break; /* previous */
    }
    if (s_state.startup_level == STARTUP_LEVEL_MIN) {
        s_state.level = LAMP_LEVEL_MIN;
    } else if (s_state.startup_level != STARTUP_LEVEL_PREVIOUS) {
        s_state.level = s_state.startup_level;
    }
    if (s_state.level < LAMP_LEVEL_MIN) s_state.level = LAMP_LEVEL_MIN;
    if (s_state.level > LAMP_LEVEL_MAX) s_state.level = LAMP_LEVEL_MAX;
}

static void log_state(const char *source)
{
    ESP_LOGI(TAG, "LIGHT on=%d level=%d src=%s", s_state.on, s_state.level, source);
}

/* ------------------------------------------------------------------------- */
/* Attribute helpers (call with the Zigbee lock held)                         */

static void write_state_attributes(bool on, uint8_t level)
{
    uint8_t on_val = on;
    ezb_zcl_set_attr_value(LAMP_ENDPOINT, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, EZB_ZCL_STD_MANUF_CODE, &on_val, false);
    ezb_zcl_set_attr_value(LAMP_ENDPOINT, EZB_ZCL_CLUSTER_ID_LEVEL, EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID, EZB_ZCL_STD_MANUF_CODE, &level, false);
}

static void write_startup_attributes(void)
{
    uint8_t su_onoff = s_state.startup_on_off;
    uint8_t su_level = s_state.startup_level;
    ezb_zcl_set_attr_value(LAMP_ENDPOINT, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_ON_OFF_START_UP_ON_OFF_ID, EZB_ZCL_STD_MANUF_CODE, &su_onoff, false);
    ezb_zcl_set_attr_value(LAMP_ENDPOINT, EZB_ZCL_CLUSTER_ID_LEVEL, EZB_ZCL_CLUSTER_SERVER,
                           EZB_ZCL_ATTR_LEVEL_START_UP_CURRENT_LEVEL_ID, EZB_ZCL_STD_MANUF_CODE, &su_level, false);
}

static bool parse_hex_key(const char *hex, uint8_t key[16])
{
    if (strlen(hex) != 32) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        unsigned int b;
        if (sscanf(hex + 2 * i, "%2x", &b) != 1) {
            return false;
        }
        key[i] = (uint8_t)b;
    }
    return true;
}

/* Standard Zigbee 3.0 default trust-center link key ("ZigBeeAlliance09"). */
static const uint8_t s_std_link_key[16] = {0x5A, 0x69, 0x67, 0x42, 0x65, 0x65, 0x41, 0x6C,
                                           0x6C, 0x69, 0x61, 0x6E, 0x63, 0x65, 0x30, 0x39};
static uint8_t s_alt_link_key[16];
static bool s_have_alt_key;
static bool s_using_alt_key;

static void select_link_key(bool alt)
{
    s_using_alt_key = alt && s_have_alt_key;
    ezb_secur_set_global_link_key(s_using_alt_key ? s_alt_link_key : s_std_link_key);
    /* Hue networks use distributed security (the parent router delivers the network key, no trust
     * center), so the alternative key is always paired with accepting distributed networks. */
#if CONFIG_LAMP_JOIN_DISTRIBUTED
    bool distributed = true;
#else
    bool distributed = s_using_alt_key;
#endif
    ezb_aps_secur_enable_distributed_security(distributed);
    ESP_LOGI(TAG, "Joining with %s link key (%s security)",
             s_using_alt_key ? "alternative (Hue/ZLL)" : "standard Zigbee 3.0", distributed ? "distributed" : "centralized");
}

/*
 * The stack holds only one global link key. When an alternative key is configured, the lamp alternates
 * between it and the standard key on successive steering attempts, so it can join both Hue bridges
 * (which deliver the network key encrypted with the ZLL link key) and standard Zigbee 3.0 coordinators.
 */
static void link_keys_init(void)
{
    const char *alt = CONFIG_LAMP_ALT_GLOBAL_LINK_KEY;
    if (alt[0]) {
        s_have_alt_key = parse_hex_key(alt, s_alt_link_key);
        if (!s_have_alt_key) {
            ESP_LOGE(TAG, "LAMP_ALT_GLOBAL_LINK_KEY must be 32 hex characters; ignoring");
        }
    }
    select_link_key(true);
}

static void send_report(uint16_t cluster_id, uint16_t attr_id)
{
    ezb_zcl_report_attr_cmd_t req = {
        .cmd_ctrl = {
            .dst_addr = { .addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = s_controller.short_addr },
            .dst_ep = s_controller.ep,
            .src_ep = LAMP_ENDPOINT,
            .cluster_id = cluster_id,
            .manuf_code = EZB_ZCL_STD_MANUF_CODE,
            .fc = { .direction = 1, .dis_default_rsp = 1 },
        },
        .payload = { .attr_id = attr_id },
    };
    ezb_err_t err = ezb_zcl_report_attr_cmd_req(&req);
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Report of 0x%04x/0x%04x failed: %d", cluster_id, attr_id, err);
    }
}

/* Coalesces local changes: a dimming ramp produces a few reports per second, the last one exact. */
static void report_timer_cb(void *arg)
{
    if (!s_joined || !s_controller.valid) {
        return;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    send_report(EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    send_report(EZB_ZCL_CLUSTER_ID_LEVEL, EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID);
    esp_zigbee_lock_release();
}

static void report_later(void)
{
    if (s_report_timer && !esp_timer_is_active(s_report_timer)) {
        esp_timer_start_once(s_report_timer, REPORT_COALESCE_US);
    }
}

/* ------------------------------------------------------------------------- */
/* Commissioning                                                              */

static void retry_timer_cb(void *arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    /* Switch keys per attempt here, not in the failure handler: that signal can arrive twice. */
    if (s_retry_mode == EZB_BDB_MODE_NETWORK_STEERING && s_have_alt_key) {
        select_link_key(!s_using_alt_key);
    }
    ezb_bdb_start_top_level_commissioning(s_retry_mode);
    esp_zigbee_lock_release();
}

static void schedule_commissioning(uint8_t mode, uint32_t delay_ms)
{
    s_retry_mode = mode;
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
}

static void log_network(const char *what)
{
    ezb_extpanid_t ext;
    ezb_nwk_get_extended_panid(&ext);
    ESP_LOGI(TAG, "%s: PAN 0x%04hx ext 0x%016llx channel %d short 0x%04hx", what, ezb_nwk_get_panid(),
             ext.u64, ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
}

static void set_joined(bool joined)
{
    s_joined = joined;
#if CONFIG_LAMP_PAIRING_INDICATION
    light_output_set_effect(joined ? LIGHT_EFFECT_NONE : LIGHT_EFFECT_PAIRING);
#endif
}

static void do_factory_reset(void *arg)
{
    ESP_LOGW(TAG, "Factory reset");
    state_erase();
    esp_zigbee_factory_reset();
}

static bool app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t type = ezb_app_signal_get_type(app_signal);

    switch (type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;

    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *(ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal);
        /* Re-assert our restored state in case the stack restored stale values. */
        portENTER_CRITICAL(&s_state_lock);
        lamp_state_t st = s_state;
        portEXIT_CRITICAL(&s_state_lock);
        write_state_attributes(st.on, st.level);
        write_startup_attributes();

        if (ezb_bdb_is_factory_new()) {
            ESP_LOGI(TAG, "Factory new: start network steering (pairing)");
            set_joined(false);
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        } else if (status == EZB_BDB_STATUS_SUCCESS) {
            set_joined(true);
            log_network("Rejoined network");
        } else {
            ESP_LOGW(TAG, "%s failed (status 0x%02x), retrying", ezb_app_signal_to_string(type), status);
            schedule_commissioning(EZB_BDB_MODE_INITIALIZATION, 1000);
        }
    } break;

    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *(ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal);
        if (status == EZB_BDB_STATUS_SUCCESS) {
            set_joined(true);
            log_network("Joined network");
        } else {
            ESP_LOGI(TAG, "Steering unsuccessful (status 0x%02x), retry in %d ms", status,
                     CONFIG_LAMP_STEERING_RETRY_MS);
            set_joined(false);
            schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING, CONFIG_LAMP_STEERING_RETRY_MS);
        }
    } break;

    case EZB_ZDO_SIGNAL_LEAVE: {
        const ezb_zdo_signal_leave_params_t *p = ezb_app_signal_get_params(app_signal);
        if (!s_joined) {
            /* The stack also signals a leave when a join attempt fails part-way (e.g. the trust
             * center never delivers the network key). That's not a removal; keep steering. */
            ESP_LOGW(TAG, "Join attempt abandoned by stack (type %d); network key not received?", p->leave_type);
            break;
        }
        ESP_LOGW(TAG, "Left network (type %d)", p->leave_type);
        if (p->leave_type == EZB_ZDO_LEAVE_TYPE_RESET) {
            /* Removed from the network (e.g. deleted in the Hue app): start over in pairing mode. */
            esp_timer_stop(s_leave_fallback_timer);
            do_factory_reset(NULL);
        }
    } break;

    default:
        ESP_LOGI(TAG, "Zigbee signal: %s (0x%02x)", ezb_app_signal_to_string(type), type);
        break;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* ZCL callbacks                                                              */

static void handle_set_attr(const ezb_zcl_set_attr_value_message_t *msg)
{
    const ezb_zcl_attribute_t *attr = &msg->in.attribute;
    if (msg->info.dst_ep != LAMP_ENDPOINT || msg->info.cluster_role != EZB_ZCL_CLUSTER_SERVER || !attr->data.value) {
        return;
    }

    bool output_changed = false;
    bool save = false;
    const char *startup_key = NULL;
    uint8_t u8 = *(uint8_t *)attr->data.value;

    portENTER_CRITICAL(&s_state_lock);
    switch (msg->info.cluster_id) {
    case EZB_ZCL_CLUSTER_ID_ON_OFF:
        if (attr->id == EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
            s_state.on = u8 != 0;
            output_changed = save = true;
        } else if (attr->id == EZB_ZCL_ATTR_ON_OFF_START_UP_ON_OFF_ID) {
            s_state.startup_on_off = u8;
            startup_key = NVS_KEY_SU_ONOFF;
        }
        break;
    case EZB_ZCL_CLUSTER_ID_LEVEL:
        if (attr->id == EZB_ZCL_ATTR_LEVEL_CURRENT_LEVEL_ID) {
            s_state.level = u8 < LAMP_LEVEL_MIN ? LAMP_LEVEL_MIN : (u8 > LAMP_LEVEL_MAX ? LAMP_LEVEL_MAX : u8);
            output_changed = save = true;
        } else if (attr->id == EZB_ZCL_ATTR_LEVEL_START_UP_CURRENT_LEVEL_ID) {
            s_state.startup_level = u8;
            startup_key = NVS_KEY_SU_LEVEL;
        }
        break;
    default:
        break;
    }
    lamp_state_t st = s_state;
    portEXIT_CRITICAL(&s_state_lock);

    if (msg->info.cluster_id == EZB_ZCL_CLUSTER_ID_IDENTIFY && attr->id == EZB_ZCL_ATTR_IDENTIFY_IDENTIFY_TIME_ID) {
        uint16_t t = *(uint16_t *)attr->data.value;
        ESP_LOGI(TAG, "Identify for %u s", t);
        light_output_identify_for((uint32_t)t * 1000);
    }

    if (output_changed) {
        light_output_set(st.on, st.level);
        log_state("remote");
    }
    if (save) {
        state_save_later();
    }
    if (startup_key) {
        startup_attr_save(startup_key, u8);
    }
}

static void handle_identify_effect(ezb_zcl_identify_effect_message_t *msg)
{
    uint32_t ms;
    switch (msg->in.effect_id) {
    case 0x00: ms = 1000; break;  /* blink */
    case 0x01: ms = 15000; break; /* breathe */
    case 0x02: ms = 2000; break;  /* okay */
    case 0x0b: ms = 8000; break;  /* channel change */
    default: ms = 0; break;       /* finish / stop */
    }
    ESP_LOGI(TAG, "Identify effect 0x%02x variant 0x%02x", msg->in.effect_id, msg->in.effect_variant);
    light_output_identify_for(ms);
    msg->out.result = EZB_ZCL_STATUS_SUCCESS;
}

#define ZCL_CMD_ON_OFF_OFF_WITH_EFFECT 0x40
#define ZCL_FC_DISABLE_DEFAULT_RSP     0x10

/*
 * esp-zigbee-lib 2.0.4 crashes (null dereference in zcl_message_scenes_store_scene) while handling
 * On/Off "Off with effect" when GlobalSceneControl is TRUE, and the Hue Bridge uses exactly that
 * command to switch lights off. Handle it here instead: switch off and answer with a Default Response.
 * GlobalSceneControl is left TRUE, so a following On is a plain On (no global-scene recall).
 */
static bool zcl_raw_frame_handler(const ezb_zcl_raw_frame_t *frame)
{
    const ezb_zcl_cmd_hdr_t *hdr = frame->header;
    bool cluster_specific = (hdr->fc & 0x03) == 0x01;
    bool to_server = !(hdr->fc & 0x08);

    /* Remember who controls us: unicast commands/reads to our endpoint from another node. */
    if (to_server && hdr->dst_ep == LAMP_ENDPOINT && hdr->src_addr.addr_mode == EZB_ADDR_MODE_SHORT &&
        hdr->dst_addr.addr_mode == EZB_ADDR_MODE_SHORT && hdr->dst_addr.u.short_addr < 0xFFF8 &&
        hdr->src_addr.u.short_addr != ezb_nwk_get_short_address() &&
        (hdr->cluster_id == EZB_ZCL_CLUSTER_ID_ON_OFF || hdr->cluster_id == EZB_ZCL_CLUSTER_ID_LEVEL)) {
        controller_remember(hdr->src_addr.u.short_addr, hdr->src_ep);
    }

    if (hdr->cluster_id != EZB_ZCL_CLUSTER_ID_ON_OFF || !cluster_specific || !to_server ||
        hdr->cmd_id != ZCL_CMD_ON_OFF_OFF_WITH_EFFECT ||
        (hdr->dst_ep != LAMP_ENDPOINT && hdr->dst_ep != EZB_AF_ENDPOINT_BROADCAST_ID)) {
        return false;
    }
    uint8_t effect = frame->payload_length > 0 ? frame->payload[0] : 0;
    uint8_t variant = frame->payload_length > 1 ? frame->payload[1] : 0;
    ESP_LOGI(TAG, "Off with effect 0x%02x/0x%02x (handled by app)", effect, variant);

    portENTER_CRITICAL(&s_state_lock);
    s_state.on = false;
    lamp_state_t st = s_state;
    portEXIT_CRITICAL(&s_state_lock);
    write_state_attributes(st.on, st.level);
    light_output_set(st.on, st.level);
    log_state("remote");
    state_save_later();

    bool unicast = hdr->dst_addr.addr_mode == EZB_ADDR_MODE_SHORT && hdr->dst_addr.u.short_addr < 0xFFF8;
    if (unicast && !(hdr->fc & ZCL_FC_DISABLE_DEFAULT_RSP)) {
        ezb_zcl_default_rsp_cmd_t rsp = {
            .cmd_ctrl = {
                .dst_addr = hdr->src_addr,
                .dst_ep = hdr->src_ep,
                .src_ep = LAMP_ENDPOINT,
                .cluster_id = hdr->cluster_id,
                .manuf_code = EZB_ZCL_STD_MANUF_CODE,
                .fc = { .direction = 1, .dis_default_rsp = 1 },
            },
            .payload = { .tsn = hdr->tsn, .rsp_to_cmd = hdr->cmd_id, .status_code = EZB_ZCL_STATUS_SUCCESS },
        };
        ezb_zcl_default_rsp_cmd_req(&rsp);
    }
    return true;
}

static void handle_scene_store(ezb_zcl_scenes_store_scene_message_t *msg)
{
    ESP_LOGI(TAG, "Store scene group 0x%04x scene %d", msg->in.group_id, msg->in.scene_id);
    msg->out.result = EZB_ZCL_STATUS_SUCCESS;
}

/* The stack keeps the scene table; applying a recalled scene's field sets is up to the application. */
static void handle_scene_recall(ezb_zcl_scenes_recall_scene_message_t *msg)
{
    portENTER_CRITICAL(&s_state_lock);
    lamp_state_t st = s_state;
    portEXIT_CRITICAL(&s_state_lock);

    for (const ezb_zcl_scenes_extension_field_t *f = msg->in.field_set; f; f = f->next) {
        if (f->cluster_id == EZB_ZCL_CLUSTER_ID_ON_OFF && f->length >= 1) {
            st.on = f->value[0] != 0;
        } else if (f->cluster_id == EZB_ZCL_CLUSTER_ID_LEVEL && f->length >= 1) {
            uint8_t l = f->value[0];
            st.level = l < LAMP_LEVEL_MIN ? LAMP_LEVEL_MIN : (l > LAMP_LEVEL_MAX ? LAMP_LEVEL_MAX : l);
        }
    }
    ESP_LOGI(TAG, "Recall scene group 0x%04x scene %d -> on=%d level=%d", msg->in.group_id, msg->in.scene_id, st.on,
             st.level);

    portENTER_CRITICAL(&s_state_lock);
    s_state.on = st.on;
    s_state.level = st.level;
    portEXIT_CRITICAL(&s_state_lock);
    write_state_attributes(st.on, st.level);
    light_output_set(st.on, st.level);
    log_state("remote");
    state_save_later();
    msg->out.result = EZB_ZCL_STATUS_SUCCESS;
}

static void zcl_action_handler(ezb_zcl_core_action_callback_id_t id, void *message)
{
    switch (id) {
    case EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID:
        handle_set_attr(message);
        break;
    case EZB_ZCL_CORE_IDENTIFY_EFFECT_CB_ID:
        handle_identify_effect(message);
        break;
    case EZB_ZCL_CORE_ON_OFF_OFF_WITH_EFFECT_CB_ID: {
        ezb_zcl_on_off_off_with_effect_message_t *msg = message;
        ESP_LOGI(TAG, "Off with effect 0x%02x variant 0x%02x", msg->in.effect_id, msg->in.effect_variant);
        msg->out.result = EZB_ZCL_STATUS_SUCCESS;
    } break;
    case EZB_ZCL_CORE_SCENES_STORE_SCENE_CB_ID:
        handle_scene_store(message);
        break;
    case EZB_ZCL_CORE_SCENES_RECALL_SCENE_CB_ID:
        handle_scene_recall(message);
        break;
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID:
        break;
    default:
        ESP_LOGI(TAG, "ZCL action 0x%04lx", (unsigned long)id);
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* Device model                                                               */

/* ZCL character strings are length-prefixed. */
static uint8_t *zcl_string(const char *s)
{
    size_t len = strlen(s);
    if (len > 32) len = 32;
    uint8_t *buf = calloc(1, len + 1);
    buf[0] = (uint8_t)len;
    memcpy(buf + 1, s, len);
    return buf;
}

static void add_attr_checked(ezb_err_t err, const char *name)
{
    if (err != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Adding attribute %s: error %d", name, err);
    }
}

static esp_err_t create_device(void)
{
    ezb_af_device_desc_t dev = ezb_af_create_device_desc();
    ezb_zha_dimmable_light_config_t cfg = EZB_ZHA_DIMMABLE_LIGHT_CONFIG();
    cfg.basic_cfg.power_source = 0x01; /* mains, single phase */
    cfg.on_off_cfg.on_off = s_state.on;
    cfg.level_cfg.current_level = s_state.level;

    ezb_af_ep_desc_t ep = ezb_zha_create_dimmable_light(LAMP_ENDPOINT, &cfg);
    ESP_RETURN_ON_FALSE(ep, ESP_FAIL, TAG, "endpoint create failed");

    ezb_zcl_cluster_desc_t basic = ezb_af_endpoint_get_cluster_desc(ep, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    add_attr_checked(ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                                         zcl_string(CONFIG_LAMP_MANUFACTURER_NAME)), "manufacturer");
    add_attr_checked(ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                                         zcl_string(CONFIG_LAMP_MODEL_ID)), "model");
    add_attr_checked(ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_SW_BUILD_ID_ID,
                                                         zcl_string(__DATE__)), "sw_build");
    static const uint8_t app_version = 1;
    add_attr_checked(ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID,
                                                         &app_version), "app_version");

    /* Extra On/Off attributes: needed for Off-with-effect and power-on behaviour (Hue). */
    ezb_zcl_cluster_desc_t on_off = ezb_af_endpoint_get_cluster_desc(ep, EZB_ZCL_CLUSTER_ID_ON_OFF, EZB_ZCL_CLUSTER_SERVER);
    static const uint8_t global_scene_control = 1;
    static const uint16_t on_time = 0, off_wait_time = 0;
    add_attr_checked(ezb_zcl_on_off_cluster_desc_add_attr(on_off, EZB_ZCL_ATTR_ON_OFF_GLOBAL_SCENE_CONTROL_ID,
                                                          &global_scene_control), "global_scene_control");
    add_attr_checked(ezb_zcl_on_off_cluster_desc_add_attr(on_off, EZB_ZCL_ATTR_ON_OFF_ON_TIME_ID, &on_time), "on_time");
    add_attr_checked(ezb_zcl_on_off_cluster_desc_add_attr(on_off, EZB_ZCL_ATTR_ON_OFF_OFF_WAIT_TIME_ID, &off_wait_time),
                     "off_wait_time");
    add_attr_checked(ezb_zcl_on_off_cluster_desc_add_attr(on_off, EZB_ZCL_ATTR_ON_OFF_START_UP_ON_OFF_ID,
                                                          &s_state.startup_on_off), "start_up_on_off");

    ezb_zcl_cluster_desc_t level = ezb_af_endpoint_get_cluster_desc(ep, EZB_ZCL_CLUSTER_ID_LEVEL, EZB_ZCL_CLUSTER_SERVER);
    static const uint8_t min_level = LAMP_LEVEL_MIN, max_level = LAMP_LEVEL_MAX;
    /* No stack-level fade on On/Off: with a fade the stack leaves CurrentLevel at MinLevel while off,
     * which would then be persisted. The output renderer already smooths on/off changes. */
    static const uint16_t on_off_transition = 0;
    add_attr_checked(ezb_zcl_level_cluster_desc_add_attr(level, EZB_ZCL_ATTR_LEVEL_MIN_LEVEL_ID, &min_level), "min_level");
    add_attr_checked(ezb_zcl_level_cluster_desc_add_attr(level, EZB_ZCL_ATTR_LEVEL_MAX_LEVEL_ID, &max_level), "max_level");
    add_attr_checked(ezb_zcl_level_cluster_desc_add_attr(level, EZB_ZCL_ATTR_LEVEL_ON_OFF_TRANSITION_TIME_ID,
                                                         &on_off_transition), "on_off_transition_time");
    add_attr_checked(ezb_zcl_level_cluster_desc_add_attr(level, EZB_ZCL_ATTR_LEVEL_START_UP_CURRENT_LEVEL_ID,
                                                         &s_state.startup_level), "start_up_current_level");

    ESP_RETURN_ON_ERROR(esp_zigbee_err_to_esp(ezb_af_device_add_endpoint_desc(dev, ep)), TAG, "add endpoint failed");
    ESP_RETURN_ON_ERROR(esp_zigbee_err_to_esp(ezb_af_device_desc_register(dev)), TAG, "register device failed");
    ezb_zcl_core_action_handler_register(zcl_action_handler);
    ezb_zcl_raw_command_handler_register(zcl_raw_frame_handler);

    /*
     * The Hue Bridge requires application device version 1. esp-zigbee-lib 2.0.4 serialises the
     * version/reserved byte of the simple descriptor with the nibbles swapped (version 1 goes on air
     * as 0x10, i.e. version 0), so store the swapped value to get 0x01 on air. Verified with a sniffer.
     */
    ezb_af_simple_desc_t *sd = (ezb_af_simple_desc_t *)ezb_af_get_simple_desc(LAMP_ENDPOINT);
    ESP_RETURN_ON_FALSE(sd, ESP_FAIL, TAG, "simple descriptor not found");
    sd->app_device_version = 0;
    sd->reserved = 1;
    return ESP_OK;
}

static void setup_security(void)
{
    link_keys_init();
}

static void zigbee_task(void *arg)
{
    esp_zigbee_config_t config = {
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_ROUTER,
            .install_code_policy = false,
            .zczr_config = { .max_children = 10 },
        },
        .platform_config = {
            .storage_partition_name = "nvs",
            .radio_config = { .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE },
        },
    };
    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    setup_security();
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(CHANNEL_MASK_ALL));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(0));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(app_signal_handler));
    ESP_ERROR_CHECK(create_device());

    ESP_ERROR_CHECK(esp_zigbee_start(false));
    esp_zigbee_launch_mainloop();

    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */

esp_err_t zigbee_light_start(void)
{
    state_load();
    state_apply_startup();
    light_output_set(s_state.on, s_state.level);
    log_state("boot");

    const esp_timer_create_args_t save_args = { .callback = state_save_now, .name = "lamp_save" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&save_args, &s_save_timer), TAG, "timer");
    const esp_timer_create_args_t retry_args = { .callback = retry_timer_cb, .name = "lamp_retry" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry_args, &s_retry_timer), TAG, "timer");
    const esp_timer_create_args_t leave_args = { .callback = do_factory_reset, .name = "lamp_leave" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&leave_args, &s_leave_fallback_timer), TAG, "timer");
    const esp_timer_create_args_t report_args = { .callback = report_timer_cb, .name = "lamp_report" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&report_args, &s_report_timer), TAG, "timer");

    /* Persist the startup-adjusted state so a later "previous" restore sees it. */
    state_save_later();

    BaseType_t ok = xTaskCreate(zigbee_task, "zigbee", 6144, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "zigbee task create failed");
    return ESP_OK;
}

void zigbee_light_get_state(bool *on, uint8_t *level)
{
    portENTER_CRITICAL(&s_state_lock);
    *on = s_state.on;
    *level = s_state.level;
    portEXIT_CRITICAL(&s_state_lock);
}

void zigbee_light_set_local(bool on, uint8_t level, bool update_attributes)
{
    if (level < LAMP_LEVEL_MIN) level = LAMP_LEVEL_MIN;
    if (level > LAMP_LEVEL_MAX) level = LAMP_LEVEL_MAX;

    portENTER_CRITICAL(&s_state_lock);
    bool changed = s_state.on != on || s_state.level != level;
    s_state.on = on;
    s_state.level = level;
    portEXIT_CRITICAL(&s_state_lock);

    light_output_set(on, level);
    if (!update_attributes) {
        return;
    }
    if (esp_zigbee_lock_acquire(pdMS_TO_TICKS(100))) {
        write_state_attributes(on, level);
        esp_zigbee_lock_release();
    } else {
        ESP_LOGW(TAG, "Zigbee lock busy; attributes not updated");
    }
    if (changed) {
        log_state("local");
    }
    report_later();
    state_save_later();
}

static void factory_reset_in_stack(void *arg)
{
    if (s_joined && !ezb_bdb_is_factory_new()) {
        /* Tell the coordinator we're leaving; the LEAVE signal completes the reset. */
        ESP_LOGW(TAG, "Leaving network before factory reset");
        esp_timer_start_once(s_leave_fallback_timer, LEAVE_FALLBACK_US);
        ezb_bdb_reset_via_local_action();
    } else {
        do_factory_reset(NULL);
    }
}

void zigbee_light_factory_reset(void)
{
    light_output_set_effect(LIGHT_EFFECT_RESET);
    if (!esp_zigbee_is_started() || esp_zigbee_task_queue_post(factory_reset_in_stack, NULL) != ESP_OK) {
        do_factory_reset(NULL);
    }
}

static void scan_result_cb(ezb_nwk_active_scan_result_t *r, void *ctx)
{
    if (!r) {
        printf("SCAN done\n");
        return;
    }
    printf("SCAN channel=%d pan=0x%04x ext=0x%016llx router=0x%04x permit_join=%d router_cap=%d profile=%d\n",
           r->channel_number, r->panid, r->extpanid.u64, r->shortaddr, r->permit_join, r->router_capacity,
           r->stack_profile);
}

esp_err_t zigbee_light_scan(void)
{
    ezb_nwk_scan_req_t req = {
        .scan_type = EZB_NWK_SCAN_TYPE_ACTIVE,
        .scan_duration = 3,
        .scan_channels = CHANNEL_MASK_ALL,
        .active_scan_cb = scan_result_cb,
    };
    esp_zigbee_lock_acquire(portMAX_DELAY);
    esp_timer_stop(s_retry_timer); /* don't start steering mid-scan; resumed below */
    ezb_err_t err = ezb_nwk_scan(&req);
    esp_zigbee_lock_release();
    if (!s_joined) {
        schedule_commissioning(EZB_BDB_MODE_NETWORK_STEERING, CONFIG_LAMP_STEERING_RETRY_MS);
    }
    return esp_zigbee_err_to_esp(err);
}

void zigbee_light_print_status(void)
{
    bool on;
    uint8_t level;
    zigbee_light_get_state(&on, &level);
    uint16_t short_addr = 0xFFFF, pan = 0xFFFF;
    uint8_t channel = 0;
    int8_t tx_power = 0;
    if (esp_zigbee_lock_acquire(pdMS_TO_TICKS(500))) {
        ezb_get_tx_power(&tx_power);
        short_addr = ezb_nwk_get_short_address();
        pan = ezb_nwk_get_panid();
        channel = ezb_nwk_get_current_channel();
        esp_zigbee_lock_release();
    }
    printf("STATUS on=%d level=%d joined=%d short=0x%04x pan=0x%04x channel=%d out=0x%06" PRIx32 " txpower=%d\n", on, level,
           s_joined, short_addr, pan, channel, light_output_get_raw(), tx_power);
}
