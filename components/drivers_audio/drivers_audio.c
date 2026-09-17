#include "drivers_audio.h"

#include <string.h>

#include "board_support.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "app_status.h"

static const char *TAG = "drivers_audio";

enum {
    AUDIO_MIC_SAMPLE_RATE_HZ = 16000,
    AUDIO_MIC_BITS_PER_SAMPLE = 32,
    AUDIO_SPEAKER_SAMPLE_RATE_HZ = 24000,
    AUDIO_SPEAKER_BITS_PER_SAMPLE = 16,
};

static bool s_ready;
static bool s_input_enabled;
static bool s_output_enabled;
static i2s_chan_handle_t s_rx_handle;
static i2s_chan_handle_t s_tx_handle;
static uint8_t s_silence_buffer[2048];

static void cleanup_channel(i2s_chan_handle_t *channel_handle, bool *enabled)
{
    if (channel_handle == NULL || *channel_handle == NULL) {
        return;
    }

    if (enabled != NULL && *enabled) {
        (void)i2s_channel_disable(*channel_handle);
        *enabled = false;
    }

    (void)i2s_del_channel(*channel_handle);
    *channel_handle = NULL;
}

static esp_err_t init_output_channel(void)
{
    const board_pins_t *pins = board_support_get_pins();
    const i2s_chan_config_t channel_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SPEAKER_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)pins->audio_bclk_gpio,
            .ws = (gpio_num_t)pins->audio_lrc_gpio,
            .dout = (gpio_num_t)pins->audio_dout_gpio,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_cfg, &s_tx_handle, NULL), TAG, "allocate speaker channel failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg), TAG, "init speaker channel failed");

    return ESP_OK;
}

static esp_err_t init_input_channel(void)
{
    const board_pins_t *pins = board_support_get_pins();
    const i2s_chan_config_t channel_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_MIC_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)pins->mic_sck_gpio,
            .ws = (gpio_num_t)pins->mic_ws_gpio,
            .dout = I2S_GPIO_UNUSED,
            .din = (gpio_num_t)pins->mic_sd_gpio,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_cfg, NULL, &s_rx_handle), TAG, "allocate mic channel failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &std_cfg), TAG, "init mic channel failed");

    return ESP_OK;
}

esp_err_t drivers_audio_init(void)
{
    esp_err_t err = ESP_OK;

    if (s_ready) {
        return ESP_OK;
    }

    err = init_output_channel();
    if (err != ESP_OK) {
        cleanup_channel(&s_tx_handle, &s_output_enabled);
        return err;
    }

    err = init_input_channel();
    if (err != ESP_OK) {
        cleanup_channel(&s_rx_handle, &s_input_enabled);
        cleanup_channel(&s_tx_handle, &s_output_enabled);
        return err;
    }

    s_ready = true;
    APP_LOGI(TAG,
             APP_STATUS_OK,
             "audio ready mic(bclk=%d ws=%d din=%d %uHz %ubit) speaker(bclk=%d ws=%d dout=%d %uHz %ubit)",
             board_support_get_pins()->mic_sck_gpio,
             board_support_get_pins()->mic_ws_gpio,
             board_support_get_pins()->mic_sd_gpio,
             (unsigned int)AUDIO_MIC_SAMPLE_RATE_HZ,
             (unsigned int)AUDIO_MIC_BITS_PER_SAMPLE,
             board_support_get_pins()->audio_bclk_gpio,
             board_support_get_pins()->audio_lrc_gpio,
             board_support_get_pins()->audio_dout_gpio,
             (unsigned int)AUDIO_SPEAKER_SAMPLE_RATE_HZ,
             (unsigned int)AUDIO_SPEAKER_BITS_PER_SAMPLE);
    return ESP_OK;
}

bool drivers_audio_is_ready(void)
{
    return s_ready;
}

esp_err_t drivers_audio_set_input_enabled(bool enabled)
{
    if (!s_ready || s_rx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_input_enabled == enabled) {
        return ESP_OK;
    }

    const esp_err_t err = enabled ? i2s_channel_enable(s_rx_handle) : i2s_channel_disable(s_rx_handle);
    if (err != ESP_OK) {
        return err;
    }

    s_input_enabled = enabled;
    APP_LOGI(TAG, APP_STATUS_OK, "mic channel %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t drivers_audio_set_output_enabled(bool enabled)
{
    if (!s_ready || s_tx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_output_enabled == enabled) {
        return ESP_OK;
    }

    const esp_err_t err = enabled ? i2s_channel_enable(s_tx_handle) : i2s_channel_disable(s_tx_handle);
    if (err != ESP_OK) {
        return err;
    }

    s_output_enabled = enabled;
    APP_LOGI(TAG, APP_STATUS_OK, "speaker channel %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t drivers_audio_read_input(void *buffer, size_t buffer_size, size_t *bytes_read, uint32_t timeout_ms)
{
    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_ready || !s_input_enabled || s_rx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2s_channel_read(s_rx_handle, buffer, buffer_size, bytes_read, pdMS_TO_TICKS(timeout_ms));
}

esp_err_t drivers_audio_write_output(const void *buffer, size_t buffer_size, size_t *bytes_written, uint32_t timeout_ms)
{
    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_ready || !s_output_enabled || s_tx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2s_channel_write(s_tx_handle, buffer, buffer_size, bytes_written, pdMS_TO_TICKS(timeout_ms));
}

esp_err_t drivers_audio_write_silence(size_t buffer_size, size_t *bytes_written, uint32_t timeout_ms)
{
    if (buffer_size == 0 || buffer_size > sizeof(s_silence_buffer)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(s_silence_buffer, 0, buffer_size);
    return drivers_audio_write_output(s_silence_buffer, buffer_size, bytes_written, timeout_ms);
}

drivers_audio_status_t drivers_audio_get_status(void)
{
    drivers_audio_status_t status = {0};

    status.mic_sample_rate_hz = AUDIO_MIC_SAMPLE_RATE_HZ;
    status.speaker_sample_rate_hz = AUDIO_SPEAKER_SAMPLE_RATE_HZ;
    status.mic_bits_per_sample = AUDIO_MIC_BITS_PER_SAMPLE;
    status.speaker_bits_per_sample = AUDIO_SPEAKER_BITS_PER_SAMPLE;
    status.input_enabled = s_input_enabled;
    status.output_enabled = s_output_enabled;

    return status;
}
