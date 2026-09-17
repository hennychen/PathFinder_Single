#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool connected;
    uint16_t speed_kmh;
    uint16_t rpm;
    int8_t coolant_temp_c;
    uint8_t throttle_percent;
    uint8_t fuel_percent;
} obd_provider_sample_t;

typedef enum {
    OBD_PROVIDER_STATUS_IDLE = 0,
    OBD_PROVIDER_STATUS_CONNECTING,
    OBD_PROVIDER_STATUS_CONNECT_FAIL,
    OBD_PROVIDER_STATUS_DEVICE_NOT_FOUND,
    OBD_PROVIDER_STATUS_GATT_MISMATCH,
    OBD_PROVIDER_STATUS_INITIALIZING,
    OBD_PROVIDER_STATUS_INIT_FAIL,
    OBD_PROVIDER_STATUS_LIVE,
    OBD_PROVIDER_STATUS_POLLING,
    OBD_PROVIDER_STATUS_POLL_WAIT,
    OBD_PROVIDER_STATUS_POLL_FAIL,
    OBD_PROVIDER_STATUS_RESPONSE_ERROR,
    OBD_PROVIDER_STATUS_PARSE_FAIL,
    OBD_PROVIDER_STATUS_WRITE_FAIL,
    OBD_PROVIDER_STATUS_INVALID_STATE,
} obd_provider_status_code_t;

enum {
    OBD_PROVIDER_NAME_MAX = 24,
    OBD_PROVIDER_STAGE_MAX = 24,
    OBD_PROVIDER_DETAIL_MAX = 64,
};

typedef struct {
    bool connected;
    uint8_t status_code;
    uint32_t connect_attempt_count;
    uint32_t failure_count;
    uint32_t update_count;
    uint32_t ble_scan_elapsed_ms;
    uint32_t ble_connect_elapsed_ms;
    uint32_t ble_discovery_elapsed_ms;
    int32_t ble_last_failure_rc;
    char backend_name[OBD_PROVIDER_NAME_MAX];
    char stage[OBD_PROVIDER_STAGE_MAX];
    char ble_last_failure_stage[OBD_PROVIDER_STAGE_MAX];
    char detail[OBD_PROVIDER_DETAIL_MAX];
} obd_provider_diagnostics_t;

esp_err_t obd_provider_init(void);
esp_err_t obd_provider_poll(obd_provider_sample_t *out_sample, bool *out_has_update);
const char *obd_provider_name(void);
void obd_provider_get_diagnostics(obd_provider_diagnostics_t *out_diag);
const char *obd_provider_status_code_to_string(obd_provider_status_code_t status_code);
