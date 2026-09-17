#include "service_attitude.h"

#include <math.h>

#include "app_state.h"
#include "app_status.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qmi8658.h"

static const char *TAG = "service_attitude";

enum {
    ATTITUDE_TASK_STACK = 4096,
    ATTITUDE_TASK_PRIORITY = 4,
    ATTITUDE_TASK_CORE = 0,
    ATTITUDE_DIAG_TASK_STACK = 3072,
    ATTITUDE_DIAG_TASK_PRIORITY = 1,
    ATTITUDE_DIAG_DELAY_MS = 3000,
    ATTITUDE_SAMPLE_PERIOD_US = 4000,
    ATTITUDE_OUTPUT_DIVIDER = 10,
    ATTITUDE_GYRO_BIAS_SAMPLES = 128,
};

typedef struct {
    float gyro_x_bias_dps;
    float gyro_y_bias_dps;
    float gyro_z_bias_dps;
} gyro_bias_t;

static TaskHandle_t s_attitude_task;
static TaskHandle_t s_attitude_diag_task;
static bool s_ready;
static gyro_bias_t s_gyro_bias;

static float clamp_float(float value, float min_value, float max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static float normalize_heading_deg(float heading_deg)
{
    while (heading_deg < 0.0f) {
        heading_deg += 360.0f;
    }
    while (heading_deg >= 360.0f) {
        heading_deg -= 360.0f;
    }
    return heading_deg;
}

static esp_err_t calibrate_gyro_bias(gyro_bias_t *out_bias)
{
    qmi8658_sample_t sample = {0};
    gyro_bias_t bias = {0};

    if (out_bias == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < ATTITUDE_GYRO_BIAS_SAMPLES; ++i) {
        ESP_RETURN_ON_ERROR(qmi8658_read_sample(&sample), TAG, "read bias sample failed");
        bias.gyro_x_bias_dps += sample.gyro_x_dps;
        bias.gyro_y_bias_dps += sample.gyro_y_dps;
        bias.gyro_z_bias_dps += sample.gyro_z_dps;
        esp_rom_delay_us(ATTITUDE_SAMPLE_PERIOD_US);
    }

    bias.gyro_x_bias_dps /= (float)ATTITUDE_GYRO_BIAS_SAMPLES;
    bias.gyro_y_bias_dps /= (float)ATTITUDE_GYRO_BIAS_SAMPLES;
    bias.gyro_z_bias_dps /= (float)ATTITUDE_GYRO_BIAS_SAMPLES;
    *out_bias = bias;
    return ESP_OK;
}

static void task_attitude(void *arg)
{
    (void)arg;

    const float radians_to_degrees = 57.2957795f;
    const float complementary_alpha = 0.98f;
    const float low_pass_alpha = 0.10f;
    float roll_deg = 0.0f;
    float pitch_deg = 0.0f;
    float filtered_roll_deg = 0.0f;
    float filtered_pitch_deg = 0.0f;
    float heading_deg = 0.0f;
    bool first_sample = true;
    uint32_t sample_counter = 0;
    int64_t last_sample_time_us = esp_timer_get_time();
    int64_t next_deadline_us = last_sample_time_us;

    while (true) {
        qmi8658_sample_t sample = {0};
        const int64_t now_us = esp_timer_get_time();
        float dt_s = (float)(now_us - last_sample_time_us) / 1000000.0f;

        last_sample_time_us = now_us;
        if (dt_s <= 0.0f || dt_s > 0.1f) {
            dt_s = (float)ATTITUDE_SAMPLE_PERIOD_US / 1000000.0f;
        }

        if (qmi8658_read_sample(&sample) == ESP_OK) {
            const float gyro_x_dps = sample.gyro_x_dps - s_gyro_bias.gyro_x_bias_dps;
            const float gyro_y_dps = sample.gyro_y_dps - s_gyro_bias.gyro_y_bias_dps;
            const float gyro_z_dps = sample.gyro_z_dps - s_gyro_bias.gyro_z_bias_dps;
            const float accel_roll_deg = atan2f(sample.accel_y_g, sample.accel_z_g) * radians_to_degrees;
            const float accel_pitch_deg =
                atan2f(-sample.accel_x_g,
                       sqrtf((sample.accel_y_g * sample.accel_y_g) + (sample.accel_z_g * sample.accel_z_g))) *
                radians_to_degrees;

            if (first_sample) {
                roll_deg = accel_roll_deg;
                pitch_deg = accel_pitch_deg;
                filtered_roll_deg = accel_roll_deg;
                filtered_pitch_deg = accel_pitch_deg;
                first_sample = false;
            } else {
                roll_deg = (complementary_alpha * (roll_deg + (gyro_x_dps * dt_s))) +
                           ((1.0f - complementary_alpha) * accel_roll_deg);
                pitch_deg = (complementary_alpha * (pitch_deg + (gyro_y_dps * dt_s))) +
                            ((1.0f - complementary_alpha) * accel_pitch_deg);
                filtered_roll_deg = (low_pass_alpha * roll_deg) + ((1.0f - low_pass_alpha) * filtered_roll_deg);
                filtered_pitch_deg = (low_pass_alpha * pitch_deg) + ((1.0f - low_pass_alpha) * filtered_pitch_deg);
            }

            filtered_roll_deg = clamp_float(filtered_roll_deg, -180.0f, 180.0f);
            filtered_pitch_deg = clamp_float(filtered_pitch_deg, -90.0f, 90.0f);
            heading_deg = normalize_heading_deg(heading_deg + (gyro_z_dps * dt_s));

            if ((sample_counter % ATTITUDE_OUTPUT_DIVIDER) == 0U) {
                app_state_set_attitude(filtered_roll_deg,
                                       filtered_pitch_deg,
                                       (uint16_t)(heading_deg + 0.5f));
            }
        }

        sample_counter++;
        next_deadline_us += ATTITUDE_SAMPLE_PERIOD_US;
        const int64_t loop_end_us = esp_timer_get_time();
        if (next_deadline_us <= loop_end_us) {
            next_deadline_us = loop_end_us + ATTITUDE_SAMPLE_PERIOD_US;
            taskYIELD();
            continue;
        }

        // #region debug-point attitude:blocking-wait
        // Was a pure busy-wait (esp_rom_delay_us + taskYIELD). taskYIELD only
        // yields to tasks of equal or higher priority, so this priority-4 task
        // starved priority-1 app_main on core 0 and the shared I2C bus lock was
        // never released in time -> ESP_ERR_TIMEOUT (0x107) in later init steps
        // (e.g. board_support_reset_lcd). Sleep coarsely in tick units first,
        // then spin only for the sub-millisecond remainder so low-priority tasks
        // get CPU while the 250Hz cadence is preserved.
        const int64_t wait_us = next_deadline_us - loop_end_us;
        const int64_t coarse_us = wait_us - 500;
        if (coarse_us >= 1000) {
            const int coarse_ticks = (int)(coarse_us / 1000);
            vTaskDelay(coarse_ticks);
        } else if (coarse_us > 0) {
            esp_rom_delay_us((uint32_t)coarse_us);
        }
        while (esp_timer_get_time() < next_deadline_us) {
            taskYIELD();
        }
        // #endregion debug-point attitude:blocking-wait
    }
}

static void task_attitude_diag(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(ATTITUDE_DIAG_DELAY_MS));
    APP_LOGW(TAG,
             APP_ERR_ATTITUDE_INIT,
             "running deferred IMU diagnostic delay=%dms after init failure",
             ATTITUDE_DIAG_DELAY_MS);
    const esp_err_t diag_err = qmi8658_run_deferred_diagnostic();
    APP_LOGW(TAG,
             APP_ERR_ATTITUDE_INIT,
             "deferred IMU diagnostic finished: %s",
             esp_err_to_name(diag_err));
    s_attitude_diag_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t service_attitude_init(void)
{
    esp_err_t err = ESP_OK;

    if (s_ready) {
        return ESP_OK;
    }

    err = qmi8658_init();
    if (err != ESP_OK) {
        if (s_attitude_diag_task == NULL) {
            BaseType_t task_ok = xTaskCreatePinnedToCore(task_attitude_diag,
                                                         "task_att_diag",
                                                         ATTITUDE_DIAG_TASK_STACK,
                                                         NULL,
                                                         ATTITUDE_DIAG_TASK_PRIORITY,
                                                         &s_attitude_diag_task,
                                                         ATTITUDE_TASK_CORE);
            if (task_ok != pdPASS) {
                APP_LOGW(TAG, APP_ERR_ATTITUDE_INIT, "create deferred IMU diagnostic task failed");
                s_attitude_diag_task = NULL;
            }
        }
        APP_LOGE(TAG, APP_ERR_IMU_INIT, "QMI8658 init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = calibrate_gyro_bias(&s_gyro_bias);
    if (err != ESP_OK) {
        APP_LOGE(TAG, APP_ERR_ATTITUDE_INIT, "gyro bias calibration failed: %s", esp_err_to_name(err));
        return err;
    }

    if (s_attitude_task == NULL) {
        BaseType_t task_ok = xTaskCreatePinnedToCore(task_attitude,
                                                     "task_attitude",
                                                     ATTITUDE_TASK_STACK,
                                                     NULL,
                                                     ATTITUDE_TASK_PRIORITY,
                                                     &s_attitude_task,
                                                     ATTITUDE_TASK_CORE);
        if (task_ok != pdPASS) {
            APP_LOGE(TAG, APP_ERR_ATTITUDE_INIT, "create attitude task failed");
            return ESP_FAIL;
        }
    }

    s_ready = true;
    APP_LOGI(TAG,
             APP_STATUS_OK,
             "attitude service ready gyro_bias=(%.3f, %.3f, %.3f) dps",
             (double)s_gyro_bias.gyro_x_bias_dps,
             (double)s_gyro_bias.gyro_y_bias_dps,
             (double)s_gyro_bias.gyro_z_bias_dps);
    return ESP_OK;
}

bool service_attitude_is_ready(void)
{
    return s_ready;
}
