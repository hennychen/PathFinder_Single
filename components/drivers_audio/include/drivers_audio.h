#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t mic_sample_rate_hz;
    uint32_t speaker_sample_rate_hz;
    uint8_t mic_bits_per_sample;
    uint8_t speaker_bits_per_sample;
    bool input_enabled;
    bool output_enabled;
} drivers_audio_status_t;

esp_err_t drivers_audio_init(void);
bool drivers_audio_is_ready(void);
esp_err_t drivers_audio_set_input_enabled(bool enabled);
esp_err_t drivers_audio_set_output_enabled(bool enabled);
esp_err_t drivers_audio_read_input(void *buffer, size_t buffer_size, size_t *bytes_read, uint32_t timeout_ms);
esp_err_t drivers_audio_write_output(const void *buffer, size_t buffer_size, size_t *bytes_written, uint32_t timeout_ms);
esp_err_t drivers_audio_write_silence(size_t buffer_size, size_t *bytes_written, uint32_t timeout_ms);
drivers_audio_status_t drivers_audio_get_status(void);
