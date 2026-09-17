#include "tca9554.h"

#include <stdbool.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "tca9554";

enum {
    TCA9554_REG_INPUT = 0x00,
    TCA9554_REG_OUTPUT = 0x01,
    TCA9554_REG_POLARITY = 0x02,
    TCA9554_REG_CONFIG = 0x03,
};

static i2c_master_dev_handle_t s_i2c_handle;
static uint8_t s_i2c_addr = 0x20;
static uint8_t s_output_cache = 0xFF;
static uint8_t s_config_cache = 0xFF;
static bool s_ready;

static uint8_t bit_mask(uint8_t bit)
{
    return (uint8_t)(1U << bit);
}

static esp_err_t normalize_pin(uint8_t exio_pin, uint8_t *out_bit)
{
    if (exio_pin < 1 || exio_pin > 8 || out_bit == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_bit = (uint8_t)(exio_pin - 1U);
    return ESP_OK;
}

static esp_err_t read_register(uint8_t reg_addr, uint8_t *out_value)
{
    if (out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_i2c_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_transmit_receive(s_i2c_handle,
                                       &reg_addr,
                                       sizeof(reg_addr),
                                       out_value,
                                       sizeof(*out_value),
                                       100);
}

static esp_err_t write_register(uint8_t reg_addr, uint8_t value)
{
    uint8_t payload[2] = {reg_addr, value};
    if (s_i2c_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_transmit(s_i2c_handle, payload, sizeof(payload), 100);
}

esp_err_t tca9554_init(i2c_master_bus_handle_t i2c_bus, uint8_t address)
{
    if (i2c_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_i2c_addr = address;

    if (s_ready) {
        return ESP_OK;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = s_i2c_addr,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &device_config, &s_i2c_handle),
                        TAG,
                        "add tca9554 i2c device failed");

    ESP_RETURN_ON_ERROR(read_register(TCA9554_REG_OUTPUT, &s_output_cache), TAG, "read output cache failed");
    ESP_RETURN_ON_ERROR(read_register(TCA9554_REG_CONFIG, &s_config_cache), TAG, "read config cache failed");
    ESP_RETURN_ON_ERROR(write_register(TCA9554_REG_POLARITY, 0x00), TAG, "disable polarity inversion failed");

    s_ready = true;
    ESP_LOGI(TAG,
             "detected IO expander at 0x%02X output=0x%02X config=0x%02X",
             s_i2c_addr,
             s_output_cache,
             s_config_cache);
    return ESP_OK;
}

esp_err_t tca9554_set_pin_mode(uint8_t exio_pin, bool output_mode)
{
    uint8_t bit = 0;
    ESP_RETURN_ON_ERROR(normalize_pin(exio_pin, &bit), TAG, "invalid pin");

    if (output_mode) {
        s_config_cache &= (uint8_t)~bit_mask(bit);
    } else {
        s_config_cache |= bit_mask(bit);
    }

    return write_register(TCA9554_REG_CONFIG, s_config_cache);
}

esp_err_t tca9554_write_pin(uint8_t exio_pin, bool high_level)
{
    uint8_t bit = 0;
    ESP_RETURN_ON_ERROR(normalize_pin(exio_pin, &bit), TAG, "invalid pin");

    if (high_level) {
        s_output_cache |= bit_mask(bit);
    } else {
        s_output_cache &= (uint8_t)~bit_mask(bit);
    }

    return write_register(TCA9554_REG_OUTPUT, s_output_cache);
}

esp_err_t tca9554_read_pin(uint8_t exio_pin, bool *out_level)
{
    uint8_t bit = 0;
    uint8_t input_value = 0;

    if (out_level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(normalize_pin(exio_pin, &bit), TAG, "invalid pin");
    ESP_RETURN_ON_ERROR(read_register(TCA9554_REG_INPUT, &input_value), TAG, "read input failed");

    *out_level = (input_value & bit_mask(bit)) != 0;
    return ESP_OK;
}

uint8_t tca9554_get_address(void)
{
    return s_i2c_addr;
}
