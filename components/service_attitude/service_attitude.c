#include "service_attitude.h"

#include <math.h>
#include <string.h>

#include "app_state.h"
#include "app_status.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
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
    ATTITUDE_MOUNT_REF_SAMPLES = 128, /* 安装基准校准采样数 (约 0.5s @250Hz) */
};

/* 安装基准 NVS 命名空间与键 */
#define ATTITUDE_NVS_NAMESPACE "pf_att"
#define ATTITUDE_NVS_KEY_GRAVITY "mount_g"
#define ATTITUDE_MOUNT_REF_MAGIC 0x50464154U /* "PFAT" */

typedef struct {
    float gyro_x_bias_dps;
    float gyro_y_bias_dps;
    float gyro_z_bias_dps;
} gyro_bias_t;

/* 安装基准: 校准姿态下的静止重力向量 (g) + 行向量旋转矩阵
 * row_x/row_y/row_z 分别为安装系 X/Y/Z 轴在传感器系中的方向向量 */
typedef struct {
    bool valid;
    float m[3][3];
} mount_ref_t;

static TaskHandle_t s_attitude_task;
static TaskHandle_t s_attitude_diag_task;
static bool s_ready;
static gyro_bias_t s_gyro_bias;
static mount_ref_t s_mount_ref;
static SemaphoreHandle_t s_mount_ref_lock;

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

static void mount_ref_lock(void)
{
    if (s_mount_ref_lock != NULL) {
        xSemaphoreTake(s_mount_ref_lock, portMAX_DELAY);
    }
}

static void mount_ref_unlock(void)
{
    if (s_mount_ref_lock != NULL) {
        xSemaphoreGive(s_mount_ref_lock);
    }
}

static void mount_ref_get(mount_ref_t *out_ref)
{
    mount_ref_lock();
    *out_ref = s_mount_ref;
    mount_ref_unlock();
}

/* 从传感器系读数 v_s 计算安装系读数 v_m = M * v_s
 * (v 为 {x,y,z} 三元组, 行主序矩阵) */
static void mount_ref_rotate(const mount_ref_t *ref, const float v_s[3], float v_m[3])
{
    if (!ref->valid) {
        v_m[0] = v_s[0];
        v_m[1] = v_s[1];
        v_m[2] = v_s[2];
        return;
    }
    for (int r = 0; r < 3; ++r) {
        v_m[r] = (ref->m[r][0] * v_s[0]) + (ref->m[r][1] * v_s[1]) + (ref->m[r][2] * v_s[2]);
    }
}

static void vec3_normalize(float v[3])
{
    const float norm = sqrtf((v[0] * v[0]) + (v[1] * v[1]) + (v[2] * v[2]));
    if (norm < 1e-6f) {
        return;
    }
    v[0] /= norm;
    v[1] /= norm;
    v[2] /= norm;
}

static void vec3_cross(const float a[3], const float b[3], float out[3])
{
    out[0] = (a[1] * b[2]) - (a[2] * b[1]);
    out[1] = (a[2] * b[0]) - (a[0] * b[2]);
    out[2] = (a[0] * b[1]) - (a[1] * b[0]);
}

static float vec3_dot(const float a[3], const float b[3])
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

/* 以校准姿态静止重力向量 g0 (指向地面, 模长 ~1g) 构造安装系正交基:
 *   Z 轴 = -g0 (校准姿态的"水平面法线", 即设备竖放时的屏幕法线)
 *   X 轴 = 传感器 X 轴去除 Z 分量后归一化 (保持航向基准不旋转)
 *   Y 轴 = Z × X, 保证正交右手系
 * 行向量 M 把传感器系读数旋转到安装系。 */
static void mount_ref_build(const float gravity_sensor[3], mount_ref_t *out_ref)
{
    float gz[3] = {-gravity_sensor[0], -gravity_sensor[1], -gravity_sensor[2]};
    vec3_normalize(gz);

    float gx[3] = {1.0f, 0.0f, 0.0f};
    const float proj = vec3_dot(gx, gz);
    gx[0] -= proj * gz[0];
    gx[1] -= proj * gz[1];
    gx[2] -= proj * gz[2];
    vec3_normalize(gx);

    float gy[3];
    vec3_cross(gz, gx, gy); /* 已单位正交, 无需再归一化 */

    out_ref->m[0][0] = gx[0];
    out_ref->m[0][1] = gx[1];
    out_ref->m[0][2] = gx[2];
    out_ref->m[1][0] = gy[0];
    out_ref->m[1][1] = gy[1];
    out_ref->m[1][2] = gy[2];
    out_ref->m[2][0] = gz[0];
    out_ref->m[2][1] = gz[1];
    out_ref->m[2][2] = gz[2];
    out_ref->valid = true;
}

static esp_err_t mount_ref_save_nvs(const mount_ref_t *ref)
{
    nvs_handle_t handle = 0;
    struct {
        uint32_t magic;
        float gravity[3];
    } blob = {.magic = ATTITUDE_MOUNT_REF_MAGIC};

    /* 存重力向量而非矩阵: 重启加载后重建正交基, 数值更稳健 */
    blob.gravity[0] = -ref->m[2][0];
    blob.gravity[1] = -ref->m[2][1];
    blob.gravity[2] = -ref->m[2][2];

    esp_err_t err = nvs_open(ATTITUDE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, ATTITUDE_NVS_KEY_GRAVITY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t mount_ref_load_nvs(mount_ref_t *out_ref)
{
    nvs_handle_t handle = 0;
    struct {
        uint32_t magic;
        float gravity[3];
    } blob = {0};

    /* NVS 可能尚未被其他服务初始化 (attitude 先于 ble 启动), 先兜底初始化 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        const esp_err_t erase_err = nvs_flash_erase();
        if (erase_err != ESP_OK) {
            return erase_err;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_open(ATTITUDE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(blob);
    err = nvs_get_blob(handle, ATTITUDE_NVS_KEY_GRAVITY, &blob, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (len != sizeof(blob) || blob.magic != ATTITUDE_MOUNT_REF_MAGIC) {
        return ESP_ERR_INVALID_CRC;
    }

    mount_ref_build(blob.gravity, out_ref);
    return out_ref->valid ? ESP_OK : ESP_FAIL;
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
            /* 安装基准: 先把加速度/陀螺仪读数旋转到安装系, 校准姿态下
             * roll/pitch 输出为 0 (兼容竖直安装场景) */
            mount_ref_t ref;
            mount_ref_get(&ref);
            const float accel_sensor[3] = {sample.accel_x_g, sample.accel_y_g, sample.accel_z_g};
            const float gyro_sensor[3] = {sample.gyro_x_dps, sample.gyro_y_dps, sample.gyro_z_dps};
            float accel_mount[3];
            float gyro_mount[3];
            mount_ref_rotate(&ref, accel_sensor, accel_mount);
            mount_ref_rotate(&ref, gyro_sensor, gyro_mount);

            const float gyro_x_dps = gyro_mount[0] - s_gyro_bias.gyro_x_bias_dps;
            const float gyro_y_dps = gyro_mount[1] - s_gyro_bias.gyro_y_bias_dps;
            const float gyro_z_dps = gyro_mount[2] - s_gyro_bias.gyro_z_bias_dps;
            const float accel_roll_deg = atan2f(accel_mount[1], accel_mount[2]) * radians_to_degrees;
            const float accel_pitch_deg =
                atan2f(-accel_mount[0],
                       sqrtf((accel_mount[1] * accel_mount[1]) + (accel_mount[2] * accel_mount[2]))) *
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
                app_state_set_imu_temperature(sample.temperature_c);
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

    if (s_mount_ref_lock == NULL) {
        s_mount_ref_lock = xSemaphoreCreateMutex();
    }

    /* 加载已记忆的安装基准: 重启后无需重新校准 */
    mount_ref_t saved_ref = {0};
    const esp_err_t load_err = mount_ref_load_nvs(&saved_ref);
    if (load_err == ESP_OK && saved_ref.valid) {
        s_mount_ref = saved_ref;
        APP_LOGI(TAG,
                 APP_STATUS_OK,
                 "mount reference restored from NVS gravity=(%.3f, %.3f, %.3f)g",
                 (double)-saved_ref.m[2][0],
                 (double)-saved_ref.m[2][1],
                 (double)-saved_ref.m[2][2]);
    } else if (load_err != ESP_ERR_NVS_NOT_FOUND) {
        APP_LOGW(TAG,
                 APP_ERR_ATTITUDE_INIT,
                 "mount reference load failed (fallback to sensor frame): %s",
                 esp_err_to_name(load_err));
    }

    app_state_set_attitude_calibrated(s_mount_ref.valid);

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

esp_err_t service_attitude_start_calibration(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 静止采样: 取安装姿态下的重力向量均值 */
    qmi8658_sample_t sample = {0};
    float gravity[3] = {0.0f, 0.0f, 0.0f};

    for (int i = 0; i < ATTITUDE_MOUNT_REF_SAMPLES; ++i) {
        ESP_RETURN_ON_ERROR(qmi8658_read_sample(&sample), TAG, "read mount sample failed");
        gravity[0] += sample.accel_x_g;
        gravity[1] += sample.accel_y_g;
        gravity[2] += sample.accel_z_g;
        esp_rom_delay_us(ATTITUDE_SAMPLE_PERIOD_US);
    }
    gravity[0] /= (float)ATTITUDE_MOUNT_REF_SAMPLES;
    gravity[1] /= (float)ATTITUDE_MOUNT_REF_SAMPLES;
    gravity[2] /= (float)ATTITUDE_MOUNT_REF_SAMPLES;

    const float norm = sqrtf((gravity[0] * gravity[0]) + (gravity[1] * gravity[1]) + (gravity[2] * gravity[2]));
    if (norm < 0.8f || norm > 1.2f) {
        APP_LOGE(TAG, APP_ERR_ATTITUDE_INIT, "mount calibration gravity norm out of range: %.3fg", (double)norm);
        return ESP_ERR_INVALID_STATE;
    }

    mount_ref_t ref;
    mount_ref_build(gravity, &ref);

    const esp_err_t err = mount_ref_save_nvs(&ref);
    if (err != ESP_OK) {
        APP_LOGE(TAG, APP_ERR_ATTITUDE_INIT, "mount calibration NVS save failed: %s", esp_err_to_name(err));
        return err;
    }

    mount_ref_lock();
    s_mount_ref = ref;
    mount_ref_unlock();

    app_state_set_attitude_calibrated(true);
    APP_LOGI(TAG,
             APP_STATUS_OK,
             "mount calibration done gravity=(%.3f, %.3f, %.3f)g norm=%.3f saved to NVS",
             (double)gravity[0],
             (double)gravity[1],
             (double)gravity[2],
             (double)norm);
    return ESP_OK;
}

bool service_attitude_has_mount_reference(void)
{
    mount_ref_t ref;
    mount_ref_get(&ref);
    return ref.valid;
}
