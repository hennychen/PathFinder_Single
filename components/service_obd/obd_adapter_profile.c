#include "obd_adapter_profile.h"

#include <string.h>

#include "sdkconfig.h"

static const obd_adapter_profile_t s_active_config_profile = {
    .profile_id = "active_sdkconfig",
    .device_name_filter = CONFIG_OBD_ELM_DEVICE_NAME,
    .service_uuid16 = CONFIG_OBD_BLE_SERVICE_UUID,
    .notify_uuid16 = CONFIG_OBD_BLE_NOTIFY_UUID,
    .write_uuid16 = CONFIG_OBD_BLE_WRITE_UUID,
#if CONFIG_OBD_BLE_WRITE_NO_RSP
    .write_without_response = true,
#else
    .write_without_response = false,
#endif
};

static const obd_adapter_profile_t s_generic_elm327_ble_profile = {
    .profile_id = "generic_elm327_ble",
    .device_name_filter = "OBDII",
    .service_uuid16 = 0xFFF0,
    .notify_uuid16 = 0xFFF1,
    .write_uuid16 = 0xFFF2,
    .write_without_response = true,
};

static const obd_adapter_profile_t s_vgate_icar_pro_ble_profile = {
    .profile_id = "vgate_icar_pro_ble",
    .device_name_filter = "VLINK",
    .service_uuid16 = 0x18F0,
    .notify_uuid16 = 0x2AF0,
    .write_uuid16 = 0x2AF1,
    .write_without_response = true,
};

static bool profile_equals(const obd_adapter_profile_t *left, const obd_adapter_profile_t *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }

    if (left->service_uuid16 != right->service_uuid16 ||
        left->notify_uuid16 != right->notify_uuid16 ||
        left->write_uuid16 != right->write_uuid16 ||
        left->write_without_response != right->write_without_response) {
        return false;
    }

    if (left->device_name_filter == NULL || right->device_name_filter == NULL) {
        return left->device_name_filter == right->device_name_filter;
    }

    return strcmp(left->device_name_filter, right->device_name_filter) == 0;
}

const obd_adapter_profile_t *obd_adapter_profile_active_config(void)
{
    return &s_active_config_profile;
}

const obd_adapter_profile_t *obd_adapter_profile_generic_elm327_ble(void)
{
    return &s_generic_elm327_ble_profile;
}

const obd_adapter_profile_t *obd_adapter_profile_vgate_icar_pro_ble(void)
{
    return &s_vgate_icar_pro_ble_profile;
}

const char *obd_adapter_profile_detect_known_family(void)
{
    if (profile_equals(&s_active_config_profile, &s_generic_elm327_ble_profile)) {
        return s_generic_elm327_ble_profile.profile_id;
    }

    if (profile_equals(&s_active_config_profile, &s_vgate_icar_pro_ble_profile)) {
        return s_vgate_icar_pro_ble_profile.profile_id;
    }

    return "custom_ble_profile";
}
