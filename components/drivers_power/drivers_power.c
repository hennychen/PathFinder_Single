#include "drivers_power.h"

#include <math.h>

#include "app_status.h"
#include "board_support.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"

static const char *TAG = "drivers_power";

enum {
    POWER_ADC_CHANNEL = ADC_CHANNEL_7,
    POWER_ADC_ATTEN = ADC_ATTEN_DB_12,
};

static const float k_measurement_offset = 0.990476f;
static const float k_voltage_divider_gain = 3.0f;

static bool s_ready;
static bool s_calibrated;
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_adc_cali_handle;

static bool init_adc_calibration(adc_unit_t unit,
                                 adc_channel_t channel,
                                 adc_atten_t atten,
                                 adc_cali_handle_t *out_handle)
{
    adc_cali_handle_t handle = NULL;
    esp_err_t ret = ESP_FAIL;
    bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = unit,
            .chan = channel,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
        calibrated = (ret == ESP_OK);
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!calibrated) {
        adc_cali_line_fitting_config_t cali_config = {
            .unit_id = unit,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
        calibrated = (ret == ESP_OK);
    }
#endif

    *out_handle = handle;
    return calibrated;
}

static uint8_t battery_percent_from_volts(float volts)
{
    if (volts <= 3.30f) {
        return 0;
    }
    if (volts >= 4.20f) {
        return 100;
    }

    // A simple Li-ion approximation that gives a gentler mid-range curve
    if (volts < 3.60f) {
        return (uint8_t)lroundf(((volts - 3.30f) / 0.30f) * 15.0f);
    }
    if (volts < 3.80f) {
        return (uint8_t)lroundf(15.0f + ((volts - 3.60f) / 0.20f) * 25.0f);
    }
    if (volts < 4.00f) {
        return (uint8_t)lroundf(40.0f + ((volts - 3.80f) / 0.20f) * 30.0f);
    }

    return (uint8_t)lroundf(70.0f + ((volts - 4.00f) / 0.20f) * 30.0f);
}

esp_err_t drivers_power_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    const adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = POWER_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle), TAG, "create adc unit failed");
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc_handle, POWER_ADC_CHANNEL, &chan_cfg), TAG, "config adc channel failed");

    s_calibrated = init_adc_calibration(ADC_UNIT_1, POWER_ADC_CHANNEL, POWER_ADC_ATTEN, &s_adc_cali_handle);
    s_ready = true;

    APP_LOGI(TAG,
             APP_STATUS_OK,
             "battery adc ready gpio=%d channel=%d calibrated=%d",
             board_support_get_pins()->battery_adc_gpio,
             POWER_ADC_CHANNEL,
             s_calibrated);
    return ESP_OK;
}

bool drivers_power_is_ready(void)
{
    return s_ready;
}

esp_err_t drivers_power_read_status(drivers_power_status_t *out_status)
{
    int raw_value = 0;
    int channel_mv = 0;
    float battery_volts = 0.0f;

    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_ready || s_adc_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc_handle, POWER_ADC_CHANNEL, &raw_value), TAG, "read battery adc failed");

    if (s_calibrated && s_adc_cali_handle != NULL) {
        ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_adc_cali_handle, raw_value, &channel_mv),
                            TAG,
                            "convert battery adc failed");
    } else {
        channel_mv = (raw_value * 3300) / 4095;
    }

    battery_volts = ((float)channel_mv * k_voltage_divider_gain / 1000.0f) / k_measurement_offset;
    out_status->battery_voltage_mv = (uint16_t)lroundf(battery_volts * 1000.0f);
    out_status->battery_percent = battery_percent_from_volts(battery_volts);
    out_status->calibrated = s_calibrated;
    return ESP_OK;
}
