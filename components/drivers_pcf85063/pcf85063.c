#include "pcf85063.h"

#include "board_support.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "pcf85063";

enum {
    PCF85063_I2C_ADDR = 0x51,
    PCF85063_REG_CTRL1 = 0x00,
    PCF85063_REG_SECONDS = 0x04,
};

static i2c_master_dev_handle_t s_i2c_handle;

static esp_err_t ensure_i2c_device(void)
{
    if (s_i2c_handle != NULL) {
        return ESP_OK;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(board_support_get_i2c_bus(), &device_config, &s_i2c_handle);
}

static uint8_t dec_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10U) << 4U) | (value % 10U));
}

static uint8_t bcd_to_dec(uint8_t value)
{
    return (uint8_t)(((value >> 4U) * 10U) + (value & 0x0FU));
}

esp_err_t pcf85063_init(void)
{
    uint8_t reg_addr = PCF85063_REG_CTRL1;
    uint8_t ctrl1 = 0;

    ESP_RETURN_ON_ERROR(ensure_i2c_device(), TAG, "create rtc i2c handle failed");

    const esp_err_t err = i2c_master_transmit_receive(s_i2c_handle,
                                                      &reg_addr,
                                                      sizeof(reg_addr),
                                                      &ctrl1,
                                                      sizeof(ctrl1),
                                                      100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "probe failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "detected RTC at 0x%02X ctrl1=0x%02X", PCF85063_I2C_ADDR, ctrl1);
    return ESP_OK;
}

esp_err_t pcf85063_read_time(pcf85063_time_t *out_time)
{
    if (out_time == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg_addr = PCF85063_REG_SECONDS;
    uint8_t raw[7] = {0};

    ESP_RETURN_ON_ERROR(ensure_i2c_device(), TAG, "create rtc i2c handle failed");
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_i2c_handle,
                                                    &reg_addr,
                                                    sizeof(reg_addr),
                                                    raw,
                                                    sizeof(raw),
                                                    100),
                        TAG,
                        "read time failed");

    out_time->second = bcd_to_dec(raw[0] & 0x7F);
    out_time->minute = bcd_to_dec(raw[1] & 0x7F);
    out_time->hour = bcd_to_dec(raw[2] & 0x3F);
    out_time->day = bcd_to_dec(raw[3] & 0x3F);
    out_time->weekday = (uint8_t)(raw[4] & 0x07);
    out_time->month = bcd_to_dec(raw[5] & 0x1F);
    out_time->year = (uint16_t)(2000U + bcd_to_dec(raw[6]));

    return ESP_OK;
}

esp_err_t pcf85063_set_time(const pcf85063_time_t *time_value)
{
    if (time_value == NULL || !pcf85063_is_time_valid(time_value)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[8] = {
        PCF85063_REG_SECONDS,
        dec_to_bcd(time_value->second),
        dec_to_bcd(time_value->minute),
        dec_to_bcd(time_value->hour),
        dec_to_bcd(time_value->day),
        (uint8_t)(time_value->weekday & 0x07),
        dec_to_bcd(time_value->month),
        dec_to_bcd((uint8_t)(time_value->year % 100U)),
    };

    ESP_RETURN_ON_ERROR(ensure_i2c_device(), TAG, "create rtc i2c handle failed");
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_i2c_handle, payload, sizeof(payload), 100),
                        TAG,
                        "set time failed");

    return ESP_OK;
}

bool pcf85063_is_time_valid(const pcf85063_time_t *time_value)
{
    if (time_value == NULL) {
        return false;
    }

    if (time_value->year < 2000U || time_value->year > 2099U) {
        return false;
    }

    if (time_value->month < 1U || time_value->month > 12U) {
        return false;
    }

    if (time_value->day < 1U || time_value->day > 31U) {
        return false;
    }

    if (time_value->weekday > 6U) {
        return false;
    }

    if (time_value->hour > 23U || time_value->minute > 59U || time_value->second > 59U) {
        return false;
    }

    return true;
}
