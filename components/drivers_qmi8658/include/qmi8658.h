#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int16_t raw_temp;
    int16_t raw_accel_x;
    int16_t raw_accel_y;
    int16_t raw_accel_z;
    int16_t raw_gyro_x;
    int16_t raw_gyro_y;
    int16_t raw_gyro_z;
    float temperature_c;
    float accel_x_g;
    float accel_y_g;
    float accel_z_g;
    float gyro_x_dps;
    float gyro_y_dps;
    float gyro_z_dps;
} qmi8658_sample_t;

esp_err_t qmi8658_init(void);
bool qmi8658_is_ready(void);
esp_err_t qmi8658_read_sample(qmi8658_sample_t *out_sample);
esp_err_t qmi8658_read_chip_id(uint8_t *out_chip_id);
esp_err_t qmi8658_run_deferred_diagnostic(void);
esp_err_t qmi8658_run_minimal_bringup(void);
int qmi8658_get_min_bringup_phase(void);
bool qmi8658_min_bringup_in_progress(void);
const char *qmi8658_get_min_bringup_phase_name(int phase);
