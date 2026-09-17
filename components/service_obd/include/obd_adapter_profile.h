#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *profile_id;
    const char *device_name_filter;
    uint16_t service_uuid16;
    uint16_t notify_uuid16;
    uint16_t write_uuid16;
    bool write_without_response;
} obd_adapter_profile_t;

const obd_adapter_profile_t *obd_adapter_profile_active_config(void);
const obd_adapter_profile_t *obd_adapter_profile_generic_elm327_ble(void);
const obd_adapter_profile_t *obd_adapter_profile_vgate_icar_pro_ble(void);
const char *obd_adapter_profile_detect_known_family(void);
