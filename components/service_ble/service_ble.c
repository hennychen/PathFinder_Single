#include "service_ble.h"

#include <string.h>

#include "app_state.h"
#include "app_status.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "os/os_mbuf.h"
#include "service_ble_protocol.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "service_ble";

static const char *k_ble_device_name = "PathFinder Nav";

static bool s_ready;
static bool s_host_synced;
static uint8_t s_ble_addr_type;
static uint16_t s_active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_att_payload_budget = BLE_ATT_DEFAULT_PAYLOAD_MAX;
static uint16_t s_nav_chr_handle;
static uint16_t s_obd_chr_handle;
static service_ble_write_handler_t s_nav_handler;
static service_ble_write_handler_t s_obd_handler;

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg);
static int nav_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);
static int obd_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);
static esp_err_t init_ble_storage(void);
static void ble_on_sync(void);
static void ble_start_advertising(void);
static void nimble_host_task(void *param);

static uint16_t bridge_payload_budget(size_t characteristic_max_size)
{
    // Query the live negotiated MTU for the active connection instead of a
    // cached event-order-dependent value: on reconnect the MTU-exchange
    // complete event can be processed before the CONNECT event callback,
    // which would wrongly reset the budget to the 20B default.
    const uint16_t conn_mtu = ble_att_mtu(s_active_conn_handle);
    const uint16_t transport_budget = (conn_mtu > BLE_ATT_WRITE_OVERHEAD)
                                          ? (uint16_t)(conn_mtu - BLE_ATT_WRITE_OVERHEAD)
                                          : BLE_ATT_DEFAULT_PAYLOAD_MAX;
    return (transport_budget < characteristic_max_size) ? transport_budget : (uint16_t)characteristic_max_size;
}

static struct ble_gatt_chr_def s_bridge_characteristics[] = {
    {
        .uuid = BLE_UUID16_DECLARE(NAVIGATION_CHARACTERISTIC_UUID16),
        .access_cb = nav_chr_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &s_nav_chr_handle,
    },
    {
        .uuid = BLE_UUID16_DECLARE(OBD_CHARACTERISTIC_UUID16),
        .access_cb = obd_chr_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &s_obd_chr_handle,
    },
    {0},
};

static const struct ble_gatt_svc_def s_bridge_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(NAVIGATION_SERVICE_UUID16),
        .characteristics = s_bridge_characteristics,
    },
    {0},
};

static esp_err_t init_ble_storage(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        const esp_err_t erase_err = nvs_flash_erase();
        if (erase_err != ESP_OK) {
            return erase_err;
        }
        err = nvs_flash_init();
    }

    return err;
}

static int submit_bridge_payload(service_ble_write_handler_t handler,
                                 uint16_t conn_handle,
                                 uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt,
                                 size_t max_size,
                                 const char *label)
{
    uint8_t payload[64] = {0};
    uint16_t payload_len = (uint16_t)sizeof(payload);
    const uint16_t allowed_len = bridge_payload_budget(max_size);
    int rc = 0;

    if (ctxt->om == NULL) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    if (handler == NULL) {
        APP_LOGW(TAG, APP_ERR_NAV_INIT, "%s handler not registered", label);
        return BLE_ATT_ERR_UNLIKELY;
    }

    rc = ble_hs_mbuf_to_flat(ctxt->om, payload, sizeof(payload), &payload_len);
    if (rc != 0) {
        APP_LOGW(TAG, APP_ERR_NAV_PARSE, "flatten %s payload failed rc=%d", label, rc);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    if (payload_len > allowed_len) {
        APP_LOGW(TAG,
                 APP_ERR_NAV_PARSE,
                 "%s payload too large len=%u allowed=%u mtu=%u attr_max=%u",
                 label,
                 payload_len,
                 allowed_len,
                 (unsigned int)(allowed_len + BLE_ATT_WRITE_OVERHEAD),
                 (unsigned int)max_size);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    const esp_err_t submit_err = handler(payload, payload_len);
    if (submit_err == ESP_OK) {
        APP_LOGI(TAG,
                 APP_STATUS_OK,
                 "%s characteristic write conn=%u attr=%u len=%u",
                 label,
                 conn_handle,
                 attr_handle,
                 payload_len);
        return 0;
    }

    APP_LOGW(TAG,
             APP_ERR_NAV_PARSE,
             "submit %s payload failed: %s",
             label,
             esp_err_to_name(submit_err));
    return (submit_err == ESP_ERR_INVALID_ARG || submit_err == ESP_ERR_INVALID_SIZE)
               ? BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN
               : BLE_ATT_ERR_UNLIKELY;
}

static int nav_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || attr_handle != s_nav_chr_handle) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    return submit_bridge_payload(s_nav_handler,
                                 conn_handle,
                                 attr_handle,
                                 ctxt,
                                 NAVIGATION_CHARACTERISTIC_PAYLOAD_MAX,
                                 "navigation");
}

static int obd_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || attr_handle != s_obd_chr_handle) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    return submit_bridge_payload(s_obd_handler,
                                 conn_handle,
                                 attr_handle,
                                 ctxt,
                                 OBD_CHARACTERISTIC_PAYLOAD_MAX,
                                 "obd");
}

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            // Note: no budget reset here. The budget is queried live via
            // ble_att_mtu() on every write, and on a reconnect the MTU event
            // may arrive before this callback, so resetting here would
            // wrongly clamp writes to the 20B default.
            s_active_conn_handle = event->connect.conn_handle;
            app_state_set_ble_phone_connected(true);
            APP_LOGI(TAG,
                     APP_STATUS_OK,
                     "ble phone connected conn=%u att_payload=%u nav_budget=%u",
                     event->connect.conn_handle,
                     s_att_payload_budget,
                     bridge_payload_budget(NAVIGATION_CHARACTERISTIC_PAYLOAD_MAX));
        } else {
            s_active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_att_payload_budget = BLE_ATT_DEFAULT_PAYLOAD_MAX;
            app_state_set_ble_phone_connected(false);
            APP_LOGW(TAG, APP_ERR_NAV_INIT, "ble connect failed status=%d", event->connect.status);
            ble_start_advertising();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_att_payload_budget = BLE_ATT_DEFAULT_PAYLOAD_MAX;
        app_state_set_ble_phone_connected(false);
        APP_LOGI(TAG,
                 APP_STATUS_OK,
                 "ble phone disconnected conn=%u reason=%d",
                 event->disconnect.conn.conn_handle,
                 event->disconnect.reason);
        ble_start_advertising();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_att_payload_budget = BLE_ATT_DEFAULT_PAYLOAD_MAX;
        app_state_set_ble_phone_connected(false);
        APP_LOGI(TAG, APP_STATUS_OK, "ble advertising complete reason=%d", event->adv_complete.reason);
        ble_start_advertising();
        return 0;
    case BLE_GAP_EVENT_MTU:
        if (event->mtu.conn_handle == s_active_conn_handle) {
            s_att_payload_budget = (event->mtu.value > BLE_ATT_WRITE_OVERHEAD)
                                       ? (uint16_t)(event->mtu.value - BLE_ATT_WRITE_OVERHEAD)
                                       : 0U;
        }
        APP_LOGI(TAG,
                 APP_STATUS_OK,
                 "ble mtu updated conn=%u mtu=%u att_payload=%u nav_budget=%u",
                 event->mtu.conn_handle,
                 event->mtu.value,
                 s_att_payload_budget,
                 bridge_payload_budget(NAVIGATION_CHARACTERISTIC_PAYLOAD_MAX));
        return 0;
    default:
        return 0;
    }
}

static void ble_start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    struct ble_gap_adv_params adv_params = {0};
    ble_uuid16_t service_uuid = BLE_UUID16_INIT(NAVIGATION_SERVICE_UUID16);

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)k_ble_device_name;
    fields.name_len = (uint8_t)strlen(k_ble_device_name);
    fields.name_is_complete = 1;
    fields.uuids16 = &service_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        APP_LOGW(TAG, APP_ERR_NAV_INIT, "set advertising fields failed rc=%d", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_ble_addr_type,
                           NULL,
                           BLE_HS_FOREVER,
                           &adv_params,
                           ble_gap_event_handler,
                           NULL);
    if (rc != 0) {
        APP_LOGW(TAG, APP_ERR_NAV_INIT, "start advertising failed rc=%d", rc);
        return;
    }

    APP_LOGI(TAG,
             APP_STATUS_OK,
             "ble advertising name=%s service=0x%04X nav_char=0x%04X obd_char=0x%04X",
             k_ble_device_name,
             NAVIGATION_SERVICE_UUID16,
             NAVIGATION_CHARACTERISTIC_UUID16,
             OBD_CHARACTERISTIC_UUID16);
}

static void ble_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        APP_LOGW(TAG, APP_ERR_NAV_INIT, "ble ensure addr failed rc=%d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_ble_addr_type);
    if (rc != 0) {
        APP_LOGW(TAG, APP_ERR_NAV_INIT, "ble infer addr type failed rc=%d", rc);
        return;
    }

    s_host_synced = true;
    ble_start_advertising();
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t service_ble_register_bridge_handlers(service_ble_write_handler_t navigation_handler,
                                               service_ble_write_handler_t obd_handler)
{
    s_nav_handler = navigation_handler;
    s_obd_handler = obd_handler;
    return ESP_OK;
}

esp_err_t service_ble_init(void)
{
    int rc = 0;

    if (s_ready) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(init_ble_storage(), TAG, "ble storage init failed");
    rc = nimble_port_init();
    if (rc != ESP_OK) {
        APP_LOGE(TAG, APP_ERR_NAV_INIT, "nimble port init failed rc=%d", rc);
        return ESP_FAIL;
    }

    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_svc_gap_device_name_set(k_ble_device_name);
    if (rc != 0) {
        return ESP_FAIL;
    }

    rc = ble_gatts_count_cfg(s_bridge_services);
    if (rc != 0) {
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_bridge_services);
    if (rc != 0) {
        return ESP_FAIL;
    }

    nimble_port_freertos_init(nimble_host_task);
    s_ready = true;
    app_state_set_ble_phone_connected(false);
    APP_LOGI(TAG,
             APP_STATUS_OK,
             "shared ble service ready name=%s service=0x%04X nav_char=0x%04X obd_char=0x%04X att_payload=%u nav_budget=%u",
             k_ble_device_name,
             NAVIGATION_SERVICE_UUID16,
             NAVIGATION_CHARACTERISTIC_UUID16,
             OBD_CHARACTERISTIC_UUID16,
             BLE_ATT_DEFAULT_PAYLOAD_MAX,
             bridge_payload_budget(NAVIGATION_CHARACTERISTIC_PAYLOAD_MAX));
    return ESP_OK;
}

bool service_ble_is_ready(void)
{
    return s_ready;
}

bool service_ble_host_is_active(void)
{
    return s_ready;
}

bool service_ble_host_is_synced(void)
{
    return s_host_synced;
}
