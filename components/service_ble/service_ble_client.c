#include "service_ble_client.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_status.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "service_ble.h"

static const char *TAG = "service_ble_cli";

enum {
    SERVICE_BLE_CLIENT_RX_BUFFER_SIZE = 1024,
    SERVICE_BLE_CLIENT_RX_TRIGGER_LEVEL = 1,
    SERVICE_BLE_CLIENT_CONNECT_MS = 30000,
    SERVICE_BLE_CLIENT_NAME_SUMMARY_MAX = 12,
};

typedef struct {
    EventGroupHandle_t ev;
    StreamBufferHandle_t rx;
    service_ble_client_profile_t profile;
    char matched_name[64];
    uint16_t service_uuid16;
    uint16_t notify_uuid16;
    uint16_t write_uuid16;
    uint16_t cccd_uuid16;
    uint8_t own_addr_type;
    uint16_t conn_handle;
    uint16_t svc_start;
    uint16_t svc_end;
    uint16_t notify_def;
    uint16_t notify_val;
    uint16_t write_val;
    uint16_t cccd_handle;
    TickType_t scan_start_tick;
    TickType_t connect_start_tick;
    TickType_t discovery_start_tick;
    bool opening;
    bool scanning;
    bool connecting;
    bool connected;
} service_ble_client_state_t;

static service_ble_client_state_t s_state = {
    .conn_handle = BLE_HS_CONN_HANDLE_NONE,
};
static service_ble_client_diagnostics_t s_diag;

static const EventBits_t BIT_READY = BIT0;
static const EventBits_t BIT_FAILED = BIT1;

static void reset_client_session(void);
static uint32_t elapsed_ms_since(TickType_t start_tick);
static void set_diag_text(char *buffer, size_t buffer_size, const char *text);
static void reset_diag_timings(void);
static void reset_diag_failures(void);
static void update_scan_elapsed(void);
static void update_connect_elapsed(void);
static void update_discovery_elapsed(void);
static void snapshot_active_phase_timings(void);
static void copy_summary_name(char *buffer, size_t buffer_size, const char *name);
static void set_diag_summary(service_ble_client_status_code_t status_code, const char *fmt, ...);
static void set_diag_status(service_ble_client_status_code_t status_code, const char *detail);
static void fail_open(service_ble_client_status_code_t status_code, const char *message, int rc);
static bool adv_name_matches(const struct ble_gap_disc_desc *disc);
static void start_scan(void);
static void subscribe_notifications(void);
static int client_gap_event(struct ble_gap_event *event, void *arg);
static int on_subscribe(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr,
                        void *arg);
static int on_disc_dsc(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       uint16_t chr_val_handle,
                       const struct ble_gatt_dsc *dsc,
                       void *arg);
static int on_disc_chr(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr,
                       void *arg);
static int on_disc_svc(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc,
                       void *arg);
static bool get_adv_name(const struct ble_gap_disc_desc *disc, char *buffer, size_t buffer_size);

static void reset_client_session(void)
{
    s_state.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_state.matched_name[0] = '\0';
    s_state.svc_start = 0;
    s_state.svc_end = 0;
    s_state.notify_def = 0;
    s_state.notify_val = 0;
    s_state.write_val = 0;
    s_state.cccd_handle = 0;
    s_state.scan_start_tick = 0;
    s_state.connect_start_tick = 0;
    s_state.discovery_start_tick = 0;
    s_state.scanning = false;
    s_state.connecting = false;
    s_state.connected = false;
    s_state.opening = false;
}

static uint32_t elapsed_ms_since(TickType_t start_tick)
{
    if (start_tick == 0) {
        return 0;
    }

    return (uint32_t)(xTaskGetTickCount() - start_tick) * portTICK_PERIOD_MS;
}

static void set_diag_text(char *buffer, size_t buffer_size, const char *text)
{
    if (buffer == NULL || buffer_size == 0U) {
        return;
    }

    if (text == NULL) {
        buffer[0] = '\0';
        return;
    }

    strncpy(buffer, text, buffer_size - 1U);
    buffer[buffer_size - 1U] = '\0';
}

static void reset_diag_timings(void)
{
    s_diag.scan_elapsed_ms = 0;
    s_diag.connect_elapsed_ms = 0;
    s_diag.discovery_elapsed_ms = 0;
}

static void reset_diag_failures(void)
{
    s_diag.last_failure_status_code = (uint8_t)SERVICE_BLE_CLIENT_STATUS_IDLE;
    s_diag.last_failure_rc = 0;
}

static void update_scan_elapsed(void)
{
    if (s_state.scan_start_tick != 0) {
        s_diag.scan_elapsed_ms = elapsed_ms_since(s_state.scan_start_tick);
    }
}

static void update_connect_elapsed(void)
{
    if (s_state.connect_start_tick != 0) {
        s_diag.connect_elapsed_ms = elapsed_ms_since(s_state.connect_start_tick);
    }
}

static void update_discovery_elapsed(void)
{
    if (s_state.discovery_start_tick != 0) {
        s_diag.discovery_elapsed_ms = elapsed_ms_since(s_state.discovery_start_tick);
    }
}

static void snapshot_active_phase_timings(void)
{
    update_scan_elapsed();
    update_connect_elapsed();
    update_discovery_elapsed();
}

static void copy_summary_name(char *buffer, size_t buffer_size, const char *name)
{
    if (buffer == NULL || buffer_size == 0U) {
        return;
    }

    if (name == NULL || name[0] == '\0') {
        buffer[0] = '\0';
        return;
    }

    if (strlen(name) <= SERVICE_BLE_CLIENT_NAME_SUMMARY_MAX) {
        set_diag_text(buffer, buffer_size, name);
        return;
    }

    snprintf(buffer,
             buffer_size,
             "%.*s~",
             SERVICE_BLE_CLIENT_NAME_SUMMARY_MAX - 1,
             name);
}

static void set_diag_summary(service_ble_client_status_code_t status_code, const char *fmt, ...)
{
    va_list args;
    char detail[SERVICE_BLE_CLIENT_DETAIL_MAX] = {0};

    va_start(args, fmt);
    vsnprintf(detail, sizeof(detail), fmt, args);
    va_end(args);
    set_diag_status(status_code, detail);
}

static void set_diag_status(service_ble_client_status_code_t status_code, const char *detail)
{
    s_diag.status_code = (uint8_t)status_code;
    s_diag.connected = s_state.connected;
    set_diag_text(s_diag.detail, sizeof(s_diag.detail), detail);
}

static void fail_open(service_ble_client_status_code_t status_code, const char *message, int rc)
{
    char detail[64] = {0};
    snapshot_active_phase_timings();
    s_diag.last_failure_status_code = (uint8_t)status_code;
    s_diag.last_failure_rc = rc;
    if (s_diag.detail[0] != '\0') {
        const int written = snprintf(detail, sizeof(detail), "%s", s_diag.detail);
        if (written < 0) {
            detail[0] = '\0';
        }
        const size_t detail_len = strnlen(detail, sizeof(detail));
        if (detail_len < sizeof(detail) - 1U) {
            snprintf(detail + detail_len, sizeof(detail) - detail_len, " | rc=%d", rc);
        }
    } else {
        snprintf(detail, sizeof(detail), "%s rc=%d", message, rc);
    }
    set_diag_status(status_code, detail);
    APP_LOGW(TAG, APP_ERR_OBD_INIT, "%s rc=%d", message, rc);
    if (s_state.ev != NULL) {
        xEventGroupSetBits(s_state.ev, BIT_FAILED);
    }
}

static bool adv_name_matches(const struct ble_gap_disc_desc *disc)
{
    char name_buf[64] = {0};

    if (disc == NULL || s_state.profile.device_name_filter == NULL || s_state.profile.device_name_filter[0] == '\0') {
        return false;
    }

    if (!get_adv_name(disc, name_buf, sizeof(name_buf))) {
        return false;
    }

    return strstr(name_buf, s_state.profile.device_name_filter) != NULL;
}

static bool get_adv_name(const struct ble_gap_disc_desc *disc, char *buffer, size_t buffer_size)
{
    struct ble_hs_adv_fields fields = {0};
    uint8_t name_len = 0;

    if (disc == NULL || buffer == NULL || buffer_size == 0U) {
        return false;
    }

    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        return false;
    }

    if (fields.name == NULL || fields.name_len == 0) {
        return false;
    }

    name_len = fields.name_len < (buffer_size - 1U) ? fields.name_len : (buffer_size - 1U);
    memcpy(buffer, fields.name, name_len);
    buffer[name_len] = '\0';
    return true;
}

static void start_scan(void)
{
    struct ble_gap_disc_params params = {0};
    int rc = 0;

    rc = ble_hs_id_infer_auto(0, &s_state.own_addr_type);
    if (rc != 0) {
        fail_open(SERVICE_BLE_CLIENT_STATUS_HOST_NOT_READY, "infer own addr type failed", rc);
        return;
    }

    params.filter_duplicates = 1;
    params.passive = 0;
    rc = ble_gap_disc(s_state.own_addr_type,
                      BLE_HS_FOREVER,
                      &params,
                      client_gap_event,
                      NULL);
    if (rc != 0) {
        fail_open(SERVICE_BLE_CLIENT_STATUS_SCAN_START_FAIL, "start client scan failed", rc);
        return;
    }

    s_state.scanning = true;
    s_state.scan_start_tick = xTaskGetTickCount();
    s_state.connect_start_tick = 0;
    s_state.discovery_start_tick = 0;
    reset_diag_timings();
    set_diag_status(SERVICE_BLE_CLIENT_STATUS_SCANNING, s_state.profile.device_name_filter);
    APP_LOGI(TAG,
             APP_STATUS_OK,
             "ble client scanning name_filter=%s svc=0x%04X notify=0x%04X write=0x%04X",
             s_state.profile.device_name_filter,
             s_state.profile.service_uuid16,
             s_state.profile.notify_uuid16,
             s_state.profile.write_uuid16);
}

static void subscribe_notifications(void)
{
    uint8_t value[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(s_state.conn_handle,
                                  s_state.cccd_handle,
                                  value,
                                  sizeof(value),
                                  on_subscribe,
                                  NULL);
    if (rc != 0) {
        fail_open(SERVICE_BLE_CLIENT_STATUS_SUBSCRIBE_FAIL, "subscribe notify failed", rc);
    }
}

static int on_subscribe(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr,
                        void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;

    if (error->status == 0) {
        update_discovery_elapsed();
        s_state.connected = true;
        s_state.opening = false;
        set_diag_summary(SERVICE_BLE_CLIENT_STATUS_CONNECTED,
                         "s=%04X-%04X n=%04X/%04X w=%04X d=%04X",
                         s_state.svc_start,
                         s_state.svc_end,
                         s_state.notify_def,
                         s_state.notify_val,
                         s_state.write_val,
                         s_state.cccd_handle);
        APP_LOGI(TAG, APP_STATUS_OK, "ble client subscribed; elm327 link ready");
        xEventGroupSetBits(s_state.ev, BIT_READY);
    } else {
        update_discovery_elapsed();
        fail_open(SERVICE_BLE_CLIENT_STATUS_SUBSCRIBE_FAIL, "subscribe callback failed", error->status);
    }

    return 0;
}

static int on_disc_dsc(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       uint16_t chr_val_handle,
                       const struct ble_gatt_dsc *dsc,
                       void *arg)
{
    (void)conn_handle;
    (void)chr_val_handle;
    (void)arg;
    const ble_uuid16_t cccd_uuid = BLE_UUID16_INIT(s_state.cccd_uuid16);

    if (error->status == 0 && dsc != NULL &&
        ble_uuid_cmp(&dsc->uuid.u, &cccd_uuid.u) == 0) {
        s_state.cccd_handle = dsc->handle;
        set_diag_summary(SERVICE_BLE_CLIENT_STATUS_CONNECTED,
                         "n=%04X/%04X w=%04X d=%04X",
                         s_state.notify_def,
                         s_state.notify_val,
                         s_state.write_val,
                         s_state.cccd_handle);
    } else if (error->status == BLE_HS_EDONE) {
        if (s_state.cccd_handle == 0) {
            char detail[64] = {0};
            update_discovery_elapsed();
            snprintf(detail, sizeof(detail), "cccd 0x%04X missing", s_state.cccd_uuid16);
            set_diag_status(SERVICE_BLE_CLIENT_STATUS_DESCRIPTOR_NOT_FOUND, detail);
            fail_open(SERVICE_BLE_CLIENT_STATUS_DESCRIPTOR_NOT_FOUND, "notify cccd not found", error->status);
        } else {
            subscribe_notifications();
        }
    } else if (error->status != 0) {
        update_discovery_elapsed();
        fail_open(SERVICE_BLE_CLIENT_STATUS_DISCOVERY_FAIL, "descriptor discovery failed", error->status);
    }

    return 0;
}

static int on_disc_chr(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr,
                       void *arg)
{
    (void)arg;
    const ble_uuid16_t notify_uuid = BLE_UUID16_INIT(s_state.notify_uuid16);
    const ble_uuid16_t write_uuid = BLE_UUID16_INIT(s_state.write_uuid16);

    if (error->status == 0 && chr != NULL) {
        if (ble_uuid_cmp(&chr->uuid.u, &notify_uuid.u) == 0) {
            s_state.notify_def = chr->def_handle;
            s_state.notify_val = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, &write_uuid.u) == 0) {
            s_state.write_val = chr->val_handle;
        }
        if (s_state.notify_val != 0 || s_state.write_val != 0) {
            set_diag_summary(SERVICE_BLE_CLIENT_STATUS_CONNECTING,
                             "n=%04X/%04X w=%04X",
                             s_state.notify_def,
                             s_state.notify_val,
                             s_state.write_val);
        }
    } else if (error->status == BLE_HS_EDONE) {
        if (s_state.notify_val == 0 || s_state.write_val == 0) {
            char detail[64] = {0};
            update_discovery_elapsed();
            snprintf(detail,
                     sizeof(detail),
                     "notify=0x%04X write=0x%04X missing",
                     s_state.notify_uuid16,
                     s_state.write_uuid16);
            set_diag_status(SERVICE_BLE_CLIENT_STATUS_CHARACTERISTIC_NOT_FOUND, detail);
            fail_open(SERVICE_BLE_CLIENT_STATUS_CHARACTERISTIC_NOT_FOUND,
                      "missing notify/write characteristic",
                      error->status);
            return 0;
        }

        ble_gattc_disc_all_dscs(conn_handle,
                                s_state.notify_def,
                                s_state.svc_end,
                                on_disc_dsc,
                                NULL);
    } else if (error->status != 0) {
        update_discovery_elapsed();
        fail_open(SERVICE_BLE_CLIENT_STATUS_DISCOVERY_FAIL, "characteristic discovery failed", error->status);
    }

    return 0;
}

static int on_disc_svc(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *svc,
                       void *arg)
{
    (void)arg;

    if (error->status == 0 && svc != NULL) {
        s_state.svc_start = svc->start_handle;
        s_state.svc_end = svc->end_handle;
        set_diag_summary(SERVICE_BLE_CLIENT_STATUS_CONNECTING,
                         "s=%04X h=%04X-%04X",
                         s_state.service_uuid16,
                         s_state.svc_start,
                         s_state.svc_end);
    } else if (error->status == BLE_HS_EDONE) {
        if (s_state.svc_start == 0) {
            char detail[64] = {0};
            update_discovery_elapsed();
            snprintf(detail, sizeof(detail), "service 0x%04X missing", s_state.service_uuid16);
            set_diag_status(SERVICE_BLE_CLIENT_STATUS_SERVICE_NOT_FOUND, detail);
            fail_open(SERVICE_BLE_CLIENT_STATUS_SERVICE_NOT_FOUND,
                      "service discovery found no matching service",
                      error->status);
            return 0;
        }

        ble_gattc_disc_all_chrs(conn_handle,
                                s_state.svc_start,
                                s_state.svc_end,
                                on_disc_chr,
                                NULL);
    } else if (error->status != 0) {
        update_discovery_elapsed();
        fail_open(SERVICE_BLE_CLIENT_STATUS_DISCOVERY_FAIL, "service discovery failed", error->status);
    }

    return 0;
}

static int client_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (adv_name_matches(&event->disc)) {
            int rc = 0;
            char adv_name[64] = {0};
            char summary_name[16] = {0};
            update_scan_elapsed();
            s_state.scanning = false;
            s_state.scan_start_tick = 0;
            s_state.connecting = true;
            s_state.connect_start_tick = xTaskGetTickCount();
            ble_gap_disc_cancel();
            if (get_adv_name(&event->disc, adv_name, sizeof(adv_name))) {
                set_diag_text(s_state.matched_name, sizeof(s_state.matched_name), adv_name);
            } else {
                set_diag_text(s_state.matched_name, sizeof(s_state.matched_name), s_state.profile.device_name_filter);
            }
            copy_summary_name(summary_name, sizeof(summary_name), s_state.matched_name);
            set_diag_summary(SERVICE_BLE_CLIENT_STATUS_DEVICE_FOUND, "dev=%s", summary_name);
            APP_LOGI(TAG, APP_STATUS_OK, "ble client found %s; connecting", s_state.matched_name);
            rc = ble_gap_connect(s_state.own_addr_type,
                                 &event->disc.addr,
                                 SERVICE_BLE_CLIENT_CONNECT_MS,
                                 NULL,
                                 client_gap_event,
                                 NULL);
            if (rc != 0) {
                s_state.connecting = false;
                fail_open(SERVICE_BLE_CLIENT_STATUS_CONNECT_FAIL, "connect to elm327 adapter failed", rc);
            } else {
                set_diag_summary(SERVICE_BLE_CLIENT_STATUS_CONNECTING, "dev=%s", summary_name);
            }
        }
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_state.scanning = false;
        if (s_state.opening && !s_state.connecting && !s_state.connected) {
            update_scan_elapsed();
            fail_open(SERVICE_BLE_CLIENT_STATUS_SCAN_TIMEOUT,
                      "ble client scan completed before match",
                      event->disc_complete.reason);
        }
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        s_state.connecting = false;
        update_connect_elapsed();
        s_state.connect_start_tick = 0;
        if (event->connect.status == 0) {
            const ble_uuid16_t service_uuid = BLE_UUID16_INIT(s_state.service_uuid16);
            char summary_name[16] = {0};
            s_state.conn_handle = event->connect.conn_handle;
            s_state.discovery_start_tick = xTaskGetTickCount();
            copy_summary_name(summary_name,
                              sizeof(summary_name),
                              s_state.matched_name[0] != '\0' ? s_state.matched_name : s_state.profile.device_name_filter);
            set_diag_summary(SERVICE_BLE_CLIENT_STATUS_CONNECTING,
                             "dev=%s c=%04X s=%04X",
                             summary_name,
                             s_state.conn_handle,
                             s_state.service_uuid16);
            ble_gattc_disc_svc_by_uuid(s_state.conn_handle, &service_uuid.u, on_disc_svc, NULL);
        } else {
            fail_open(SERVICE_BLE_CLIENT_STATUS_CONNECT_FAIL, "ble client connect callback failed", event->connect.status);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        snapshot_active_phase_timings();
        s_state.connected = false;
        s_state.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        set_diag_status(SERVICE_BLE_CLIENT_STATUS_DISCONNECTED, "gap disconnect");
        APP_LOGW(TAG, APP_ERR_OBD_INIT, "ble client disconnected reason=%d", event->disconnect.reason);
        if (s_state.opening) {
            xEventGroupSetBits(s_state.ev, BIT_FAILED);
        }
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (s_state.rx != NULL && event->notify_rx.om != NULL) {
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            uint8_t temp[256] = {0};
            if (len > sizeof(temp)) {
                len = sizeof(temp);
            }
            if (ble_hs_mbuf_to_flat(event->notify_rx.om, temp, len, &len) == 0) {
                xStreamBufferSend(s_state.rx, temp, len, 0);
            }
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        APP_LOGI(TAG,
                 APP_STATUS_OK,
                 "ble client mtu updated conn=%u mtu=%u",
                 event->mtu.conn_handle,
                 event->mtu.value);
        return 0;
    default:
        return 0;
    }
}

esp_err_t service_ble_client_open(const service_ble_client_profile_t *profile, uint32_t timeout_ms)
{
    EventBits_t bits = 0;

    if (profile == NULL || profile->device_name_filter == NULL || profile->device_name_filter[0] == '\0') {
        reset_diag_timings();
        s_diag.last_failure_status_code = (uint8_t)SERVICE_BLE_CLIENT_STATUS_INVALID_PROFILE;
        s_diag.last_failure_rc = ESP_ERR_INVALID_ARG;
        set_diag_status(SERVICE_BLE_CLIENT_STATUS_INVALID_PROFILE, "invalid device_name_filter");
        return ESP_ERR_INVALID_ARG;
    }

    if (!service_ble_host_is_active() || !service_ble_host_is_synced()) {
        reset_diag_timings();
        s_diag.last_failure_status_code = (uint8_t)SERVICE_BLE_CLIENT_STATUS_HOST_NOT_READY;
        s_diag.last_failure_rc = ESP_ERR_INVALID_STATE;
        set_diag_status(SERVICE_BLE_CLIENT_STATUS_HOST_NOT_READY, "shared host not ready");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state.ev == NULL) {
        s_state.ev = xEventGroupCreate();
    }
    if (s_state.rx == NULL) {
        s_state.rx = xStreamBufferCreate(SERVICE_BLE_CLIENT_RX_BUFFER_SIZE,
                                         SERVICE_BLE_CLIENT_RX_TRIGGER_LEVEL);
    }
    if (s_state.ev == NULL || s_state.rx == NULL) {
        reset_diag_timings();
        s_diag.last_failure_status_code = (uint8_t)SERVICE_BLE_CLIENT_STATUS_NO_MEMORY;
        s_diag.last_failure_rc = ESP_ERR_NO_MEM;
        set_diag_status(SERVICE_BLE_CLIENT_STATUS_NO_MEMORY, "alloc diagnostics buffer failed");
        return ESP_ERR_NO_MEM;
    }

    service_ble_client_close();
    reset_client_session();
    xEventGroupClearBits(s_state.ev, BIT_READY | BIT_FAILED);
    xStreamBufferReset(s_state.rx);

    s_state.profile = *profile;
    s_state.service_uuid16 = profile->service_uuid16;
    s_state.notify_uuid16 = profile->notify_uuid16;
    s_state.write_uuid16 = profile->write_uuid16;
    s_state.cccd_uuid16 = 0x2902;
    s_state.opening = true;
    reset_diag_timings();
    reset_diag_failures();
    set_diag_status(SERVICE_BLE_CLIENT_STATUS_IDLE, "client session reset");

    start_scan();
    bits = xEventGroupWaitBits(s_state.ev,
                               BIT_READY | BIT_FAILED,
                               pdFALSE,
                               pdFALSE,
                               pdMS_TO_TICKS(timeout_ms));
    if ((bits & BIT_READY) != 0) {
        return ESP_OK;
    }

    if ((bits & BIT_FAILED) == 0) {
        snapshot_active_phase_timings();
        s_diag.last_failure_status_code = (uint8_t)SERVICE_BLE_CLIENT_STATUS_SCAN_TIMEOUT;
        s_diag.last_failure_rc = ESP_ERR_TIMEOUT;
        set_diag_status(SERVICE_BLE_CLIENT_STATUS_SCAN_TIMEOUT, "ble client open timed out");
    }
    service_ble_client_close();
    return (bits & BIT_FAILED) != 0 ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

void service_ble_client_close(void)
{
    if (s_state.scanning) {
        ble_gap_disc_cancel();
        s_state.scanning = false;
    }

    if (s_state.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_state.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    reset_client_session();
}

bool service_ble_client_is_connected(void)
{
    return s_state.connected;
}

int service_ble_client_read(uint8_t *buffer, size_t capacity, uint32_t timeout_ms)
{
    if (buffer == NULL || capacity == 0 || s_state.rx == NULL) {
        return 0;
    }

    return (int)xStreamBufferReceive(s_state.rx, buffer, capacity, pdMS_TO_TICKS(timeout_ms));
}

esp_err_t service_ble_client_write(const uint8_t *buffer, size_t size)
{
    int rc = 0;

    if (buffer == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_state.connected || s_state.conn_handle == BLE_HS_CONN_HANDLE_NONE || s_state.write_val == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    if (size > UINT16_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (s_state.profile.write_without_response) {
        rc = ble_gattc_write_no_rsp_flat(s_state.conn_handle,
                                         s_state.write_val,
                                         buffer,
                                         (uint16_t)size);
    } else {
        rc = ble_gattc_write_flat(s_state.conn_handle,
                                  s_state.write_val,
                                  buffer,
                                  (uint16_t)size,
                                  NULL,
                                  NULL);
    }

    return rc == 0 ? ESP_OK : ESP_FAIL;
}

void service_ble_client_get_diagnostics(service_ble_client_diagnostics_t *out_diag)
{
    if (out_diag == NULL) {
        return;
    }

    *out_diag = s_diag;
    out_diag->connected = s_state.connected;
}

const char *service_ble_client_status_code_to_string(service_ble_client_status_code_t status_code)
{
    switch (status_code) {
    case SERVICE_BLE_CLIENT_STATUS_SCANNING:
        return "scanning";
    case SERVICE_BLE_CLIENT_STATUS_DEVICE_FOUND:
        return "device_found";
    case SERVICE_BLE_CLIENT_STATUS_CONNECTING:
        return "connecting";
    case SERVICE_BLE_CLIENT_STATUS_CONNECTED:
        return "connected";
    case SERVICE_BLE_CLIENT_STATUS_SCAN_TIMEOUT:
        return "scan_timeout";
    case SERVICE_BLE_CLIENT_STATUS_CONNECT_FAIL:
        return "connect_fail";
    case SERVICE_BLE_CLIENT_STATUS_SERVICE_NOT_FOUND:
        return "service_not_found";
    case SERVICE_BLE_CLIENT_STATUS_CHARACTERISTIC_NOT_FOUND:
        return "characteristic_not_found";
    case SERVICE_BLE_CLIENT_STATUS_DESCRIPTOR_NOT_FOUND:
        return "descriptor_not_found";
    case SERVICE_BLE_CLIENT_STATUS_SUBSCRIBE_FAIL:
        return "subscribe_fail";
    case SERVICE_BLE_CLIENT_STATUS_HOST_NOT_READY:
        return "host_not_ready";
    case SERVICE_BLE_CLIENT_STATUS_INVALID_PROFILE:
        return "invalid_profile";
    case SERVICE_BLE_CLIENT_STATUS_NO_MEMORY:
        return "no_memory";
    case SERVICE_BLE_CLIENT_STATUS_SCAN_START_FAIL:
        return "scan_start_fail";
    case SERVICE_BLE_CLIENT_STATUS_DISCOVERY_FAIL:
        return "discovery_fail";
    case SERVICE_BLE_CLIENT_STATUS_DISCONNECTED:
        return "disconnected";
    case SERVICE_BLE_CLIENT_STATUS_IDLE:
    default:
        return "idle";
    }
}
