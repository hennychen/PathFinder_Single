#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    SERVICE_BLE_CLIENT_STATUS_IDLE = 0,
    SERVICE_BLE_CLIENT_STATUS_SCANNING,
    SERVICE_BLE_CLIENT_STATUS_DEVICE_FOUND,
    SERVICE_BLE_CLIENT_STATUS_CONNECTING,
    SERVICE_BLE_CLIENT_STATUS_CONNECTED,
    SERVICE_BLE_CLIENT_STATUS_SCAN_TIMEOUT,
    SERVICE_BLE_CLIENT_STATUS_CONNECT_FAIL,
    SERVICE_BLE_CLIENT_STATUS_SERVICE_NOT_FOUND,
    SERVICE_BLE_CLIENT_STATUS_CHARACTERISTIC_NOT_FOUND,
    SERVICE_BLE_CLIENT_STATUS_DESCRIPTOR_NOT_FOUND,
    SERVICE_BLE_CLIENT_STATUS_SUBSCRIBE_FAIL,
    SERVICE_BLE_CLIENT_STATUS_HOST_NOT_READY,
    SERVICE_BLE_CLIENT_STATUS_INVALID_PROFILE,
    SERVICE_BLE_CLIENT_STATUS_NO_MEMORY,
    SERVICE_BLE_CLIENT_STATUS_SCAN_START_FAIL,
    SERVICE_BLE_CLIENT_STATUS_DISCOVERY_FAIL,
    SERVICE_BLE_CLIENT_STATUS_DISCONNECTED,
} service_ble_client_status_code_t;

enum {
    SERVICE_BLE_CLIENT_DETAIL_MAX = 64,
};

typedef struct {
    const char *device_name_filter;
    uint16_t service_uuid16;
    uint16_t notify_uuid16;
    uint16_t write_uuid16;
    bool write_without_response;
} service_ble_client_profile_t;

typedef struct {
    bool connected;
    uint8_t status_code;
    uint32_t scan_elapsed_ms;
    uint32_t connect_elapsed_ms;
    uint32_t discovery_elapsed_ms;
    uint8_t last_failure_status_code;
    int32_t last_failure_rc;
    char detail[SERVICE_BLE_CLIENT_DETAIL_MAX];
} service_ble_client_diagnostics_t;

esp_err_t service_ble_client_open(const service_ble_client_profile_t *profile, uint32_t timeout_ms);
void service_ble_client_close(void);
bool service_ble_client_is_connected(void);
int service_ble_client_read(uint8_t *buffer, size_t capacity, uint32_t timeout_ms);
esp_err_t service_ble_client_write(const uint8_t *buffer, size_t size);
void service_ble_client_get_diagnostics(service_ble_client_diagnostics_t *out_diag);
const char *service_ble_client_status_code_to_string(service_ble_client_status_code_t status_code);
