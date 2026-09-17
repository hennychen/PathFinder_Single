#include "qmi8658.h"

#include <string.h>

#include "board_support.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "qmi8658";

enum {
    QMI8658_REG_WHO_AM_I = 0x00,
    QMI8658_REG_REVISION_ID = 0x01,
    QMI8658_REG_CTRL1 = 0x02,
    QMI8658_REG_CTRL2 = 0x03,
    QMI8658_REG_CTRL3 = 0x04,
    QMI8658_REG_CTRL5 = 0x06,
    QMI8658_REG_CTRL6 = 0x07,
    QMI8658_REG_CTRL7 = 0x08,
    QMI8658_REG_TEMP_L = 0x33,
    QMI8658_WHO_AM_I_VALUE = 0x05,
    QMI8658_READ_LEN = 14,
    QMI8658_I2C_SPEED_HZ = 400000,
    QMI8658_DIAG_I2C_SPEED_HZ = 100000,
    QMI8658_POWER_ON_SETTLE_US = 120000,
    QMI8658_PROBE_RETRY_COUNT = 8,
    QMI8658_PROBE_RETRY_DELAY_US = 20000,
    QMI8658_SAFE_PROBE_TIMEOUT_MS = 20,
};

static bool s_ready;
static uint8_t s_i2c_addr;

// #region debug-point qmi-min-bringup:phase-tracker
// Minimal bring-up phase tracker: written before each I2C transaction,
// read by the app_main line observer when the bring-up task appears hung.
static volatile enum {
    QMI_MIN_PHASE_NONE = 0,
    QMI_MIN_PHASE_SETTLE,
    QMI_MIN_PHASE_DETECT_RETRY,
    QMI_MIN_PHASE_DETECT_DUMP,
    QMI_MIN_PHASE_READ_REVISION,
    QMI_MIN_PHASE_WRITE_CTRL7,
    QMI_MIN_PHASE_WRITE_CTRL6,
    QMI_MIN_PHASE_READBACK_WHOAMI,
    QMI_MIN_PHASE_READBACK_REVISION,
    QMI_MIN_PHASE_READBACK_CTRL7,
    QMI_MIN_PHASE_READBACK_CTRL6,
    QMI_MIN_PHASE_DONE,
} s_min_bringup_phase = QMI_MIN_PHASE_NONE;
// #endregion debug-point qmi-min-bringup:phase-tracker

static const uint8_t k_qmi8658_i2c_addresses[] = {
    0x6B,
};

static const uint8_t k_qmi8658_diag_addresses[] = {
    0x6B,
    0x6A,
};

static esp_err_t open_i2c_device_at_address_with_speed(uint8_t address,
                                                       uint32_t scl_speed_hz,
                                                       i2c_master_dev_handle_t *out_handle)
{
    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = scl_speed_hz,
    };
    *out_handle = NULL;
    return i2c_master_bus_add_device(board_support_get_i2c_bus(), &device_config, out_handle);
}

static esp_err_t open_i2c_device_at_address(uint8_t address, i2c_master_dev_handle_t *out_handle)
{
    return open_i2c_device_at_address_with_speed(address, QMI8658_I2C_SPEED_HZ, out_handle);
}

static esp_err_t open_i2c_device(i2c_master_dev_handle_t *out_handle)
{
    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_i2c_addr == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    return open_i2c_device_at_address(s_i2c_addr, out_handle);
}

static void close_i2c_device(i2c_master_dev_handle_t handle)
{
    if (handle == NULL) {
        return;
    }
    // #region debug-point qmi:rm-device-retry
    // i2c_master_bus_rm_device() refuses to run while the bus status is
    // READ/READ_ALL/WRITE/START (i.e. another device's transaction is in
    // flight on this shared bus) and returns ESP_ERR_INVALID_STATE WITHOUT
    // freeing the handle — a permanent leak at 250Hz sampling. Retry a few
    // times; only report if it truly cannot be released. (debug-spd2010-touch.md H3)
    esp_err_t rm_err = i2c_master_bus_rm_device(handle);
    for (int attempt = 0; rm_err == ESP_ERR_INVALID_STATE && attempt < 5; ++attempt) {
        esp_rom_delay_us(200);
        rm_err = i2c_master_bus_rm_device(handle);
    }
    if (rm_err != ESP_OK) {
        ESP_LOGW(TAG, "release qmi8658 i2c handle failed: %s (handle leaked)", esp_err_to_name(rm_err));
    }
    // #endregion
}

static esp_err_t read_register(uint8_t reg_addr, uint8_t *out_value)
{
    if (out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_dev_handle_t handle = NULL;
    ESP_RETURN_ON_ERROR(open_i2c_device(&handle), TAG, "open qmi8658 i2c handle failed");
    const esp_err_t err = i2c_master_transmit_receive(handle, &reg_addr, sizeof(reg_addr), out_value, 1, 100);
    close_i2c_device(handle);
    return err;
}

static esp_err_t read_block(uint8_t start_reg, uint8_t *out_data, size_t data_len)
{
    if (out_data == NULL || data_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_dev_handle_t handle = NULL;
    ESP_RETURN_ON_ERROR(open_i2c_device(&handle), TAG, "open qmi8658 i2c handle failed");
    const esp_err_t err = i2c_master_transmit_receive(handle, &start_reg, sizeof(start_reg), out_data, data_len, 100);
    close_i2c_device(handle);
    return err;
}

static int16_t decode_i16(const uint8_t *data)
{
    return (int16_t)((((uint16_t)data[1]) << 8) | data[0]);
}

esp_err_t qmi8658_read_chip_id(uint8_t *out_chip_id)
{
    return read_register(QMI8658_REG_WHO_AM_I, out_chip_id);
}

static esp_err_t probe_chip_id_at_address(uint8_t address, uint8_t *out_chip_id)
{
    esp_err_t err = ESP_OK;
    i2c_master_dev_handle_t probe_handle = NULL;
    uint8_t chip_id = 0;
    uint8_t last_chip_id = 0;
    uint8_t reg_dump[9] = {0};
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = QMI8658_I2C_SPEED_HZ,
    };

    ESP_RETURN_ON_FALSE(out_chip_id != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid chip id output");
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_support_get_i2c_bus(), &device_config, &probe_handle),
                        TAG,
                        "add qmi8658 probe device failed");

    for (int attempt = 1; attempt <= QMI8658_PROBE_RETRY_COUNT; ++attempt) {
        chip_id = 0;
        err = i2c_master_transmit_receive(probe_handle,
                                          (uint8_t[]){QMI8658_REG_WHO_AM_I},
                                          1,
                                          &chip_id,
                                          1,
                                          100);
        if (err == ESP_OK && chip_id == QMI8658_WHO_AM_I_VALUE) {
            break;
        }

        if (err == ESP_OK && chip_id != 0x00U && chip_id != last_chip_id) {
            ESP_LOGW(TAG, "probe addr=0x%02X attempt=%d transitional chip id 0x%02X", address, attempt, chip_id);
            last_chip_id = chip_id;
        } else if (err != ESP_OK && attempt == QMI8658_PROBE_RETRY_COUNT) {
            ESP_LOGW(TAG, "probe addr=0x%02X failed after %d attempts: %s", address, attempt, esp_err_to_name(err));
        }

        if (attempt < QMI8658_PROBE_RETRY_COUNT) {
            esp_rom_delay_us(QMI8658_PROBE_RETRY_DELAY_US);
        }
    }

    if (err == ESP_OK && chip_id != QMI8658_WHO_AM_I_VALUE) {
        const esp_err_t dump_err = i2c_master_transmit_receive(probe_handle,
                                                               (uint8_t[]){QMI8658_REG_WHO_AM_I},
                                                               1,
                                                               reg_dump,
                                                               sizeof(reg_dump),
                                                               100);
        if (dump_err == ESP_OK) {
            ESP_LOGW(TAG,
                     "probe addr=0x%02X regs[00..08]=%02X %02X %02X %02X %02X %02X %02X %02X %02X",
                     address,
                     reg_dump[0],
                     reg_dump[1],
                     reg_dump[2],
                     reg_dump[3],
                     reg_dump[4],
                     reg_dump[5],
                     reg_dump[6],
                     reg_dump[7],
                     reg_dump[8]);
        }
    }

    if (err != ESP_OK) {
        i2c_master_bus_rm_device(probe_handle);
        return err;
    }

    i2c_master_bus_rm_device(probe_handle);
    *out_chip_id = chip_id;
    return ESP_OK;
}

static esp_err_t probe_read_register_at_address_with_timeout(uint8_t address,
                                                             uint8_t reg_addr,
                                                             uint8_t *out_value,
                                                             int timeout_ms)
{
    esp_err_t err = ESP_OK;
    i2c_master_dev_handle_t probe_handle = NULL;
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = QMI8658_I2C_SPEED_HZ,
    };

    ESP_RETURN_ON_FALSE(out_value != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid register output");
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_support_get_i2c_bus(), &device_config, &probe_handle),
                        TAG,
                        "add qmi8658 register probe device failed");

    err = i2c_master_transmit_receive(probe_handle,
                                      (uint8_t[]){reg_addr},
                                      1,
                                      out_value,
                                      1,
                                      timeout_ms);
    i2c_master_bus_rm_device(probe_handle);
    return err;
}

static esp_err_t read_register_with_handle(i2c_master_dev_handle_t handle,
                                           uint8_t reg_addr,
                                           uint8_t *out_value,
                                           int timeout_ms)
{
    if (handle == NULL || out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return i2c_master_transmit_receive(handle, &reg_addr, sizeof(reg_addr), out_value, 1, timeout_ms);
}

static esp_err_t write_register_at_address_with_timeout(uint8_t address,
                                                        uint8_t reg_addr,
                                                        uint8_t value,
                                                        int timeout_ms)
{
    esp_err_t err = ESP_OK;
    i2c_master_dev_handle_t handle = NULL;
    uint8_t payload[2] = {reg_addr, value};

    ESP_RETURN_ON_ERROR(open_i2c_device_at_address(address, &handle), TAG, "open qmi8658 write handle failed");
    err = i2c_master_transmit(handle, payload, sizeof(payload), timeout_ms);
    close_i2c_device(handle);
    return err;
}

static esp_err_t transmit_register_address_with_handle(i2c_master_dev_handle_t handle,
                                                       uint8_t reg_addr,
                                                       int timeout_ms)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return i2c_master_transmit(handle, &reg_addr, sizeof(reg_addr), timeout_ms);
}

static esp_err_t receive_register_value_with_handle(i2c_master_dev_handle_t handle,
                                                    uint8_t *out_value,
                                                    int timeout_ms)
{
    if (handle == NULL || out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return i2c_master_receive(handle, out_value, 1, timeout_ms);
}

static void log_deferred_diag_bus_state(uint8_t address, int pass)
{
    ESP_LOGW(TAG,
             "deferred diag bus state: bus=%p addr=0x%02X pass=%d speed=%lu",
             (void *)board_support_get_i2c_bus(),
             address,
             pass,
             (unsigned long)QMI8658_DIAG_I2C_SPEED_HZ);
}

static esp_err_t detect_qmi8658_address(uint8_t *out_address, uint8_t *out_chip_id)
{
    ESP_RETURN_ON_FALSE(out_address != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid detected address output");
    ESP_RETURN_ON_FALSE(out_chip_id != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid detected chip id output");

    for (size_t i = 0; i < (sizeof(k_qmi8658_i2c_addresses) / sizeof(k_qmi8658_i2c_addresses[0])); ++i) {
        const uint8_t address = k_qmi8658_i2c_addresses[i];
        uint8_t chip_id = 0;
        const esp_err_t err = probe_chip_id_at_address(address, &chip_id);
        if (err == ESP_OK) {
            *out_address = address;
            *out_chip_id = chip_id;
            if (chip_id == QMI8658_WHO_AM_I_VALUE) {
                ESP_LOGI(TAG, "detected QMI8658 addr=0x%02X chip=0x%02X", address, chip_id);
            } else {
                ESP_LOGW(TAG, "detected IMU bus response addr=0x%02X raw chip id=0x%02X", address, chip_id);
            }
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t qmi8658_run_deferred_diagnostic(void)
{
    esp_err_t final_err = ESP_ERR_NOT_FOUND;

    ESP_LOGW(TAG,
             "deferred diag start: probing WHO_AM_I with persistent per-address handles after boot speed=%lu",
             (unsigned long)QMI8658_DIAG_I2C_SPEED_HZ);
    for (size_t i = 0; i < (sizeof(k_qmi8658_diag_addresses) / sizeof(k_qmi8658_diag_addresses[0])); ++i) {
        const uint8_t address = k_qmi8658_diag_addresses[i];
        i2c_master_dev_handle_t diag_handle = NULL;
        ESP_LOGW(TAG, "deferred diag open: addr=0x%02X speed=%lu", address, (unsigned long)QMI8658_DIAG_I2C_SPEED_HZ);
        const esp_err_t open_err =
            open_i2c_device_at_address_with_speed(address, QMI8658_DIAG_I2C_SPEED_HZ, &diag_handle);
        if (open_err != ESP_OK) {
            ESP_LOGW(TAG, "deferred diag open failed: addr=0x%02X err=%s", address, esp_err_to_name(open_err));
            continue;
        }

        for (int pass = 1; pass <= 2; ++pass) {
            uint8_t safe_probe_chip_id = 0;
            ESP_LOGW(TAG,
                     "deferred diag step: addr=0x%02X pass=%d timeout=%dms mode=persistent_handle speed=%lu",
                     address,
                     pass,
                     QMI8658_SAFE_PROBE_TIMEOUT_MS,
                     (unsigned long)QMI8658_DIAG_I2C_SPEED_HZ);
            log_deferred_diag_bus_state(address, pass);
            if (pass == 1) {
                ESP_LOGW(TAG, "deferred diag bus reset: addr=0x%02X pass=%d", address, pass);
                const esp_err_t reset_err = i2c_master_bus_reset(board_support_get_i2c_bus());
                ESP_LOGW(TAG,
                         "deferred diag bus reset result: addr=0x%02X pass=%d err=%s",
                         address,
                         pass,
                         esp_err_to_name(reset_err));
                ESP_LOGW(TAG,
                         "deferred diag split tx: addr=0x%02X pass=%d reg=0x%02X timeout=%dms",
                         address,
                         pass,
                         QMI8658_REG_WHO_AM_I,
                         QMI8658_SAFE_PROBE_TIMEOUT_MS);
                const esp_err_t split_tx_err =
                    transmit_register_address_with_handle(diag_handle,
                                                          QMI8658_REG_WHO_AM_I,
                                                          QMI8658_SAFE_PROBE_TIMEOUT_MS);
                ESP_LOGW(TAG,
                         "deferred diag split tx result: addr=0x%02X pass=%d err=%s",
                         address,
                         pass,
                         esp_err_to_name(split_tx_err));
                if (split_tx_err == ESP_OK) {
                    uint8_t split_rx_chip_id = 0;
                    ESP_LOGW(TAG,
                             "deferred diag split rx: addr=0x%02X pass=%d len=1 timeout=%dms",
                             address,
                             pass,
                             QMI8658_SAFE_PROBE_TIMEOUT_MS);
                    const esp_err_t split_rx_err =
                        receive_register_value_with_handle(diag_handle,
                                                           &split_rx_chip_id,
                                                           QMI8658_SAFE_PROBE_TIMEOUT_MS);
                    ESP_LOGW(TAG,
                             "deferred diag split rx result: addr=0x%02X pass=%d err=%s probe=0x%02X",
                             address,
                             pass,
                             esp_err_to_name(split_rx_err),
                             split_rx_chip_id);
                }
            }
            const esp_err_t safe_probe_err =
                read_register_with_handle(diag_handle,
                                          QMI8658_REG_WHO_AM_I,
                                          &safe_probe_chip_id,
                                          QMI8658_SAFE_PROBE_TIMEOUT_MS);
            if (safe_probe_err == ESP_OK) {
                final_err = ESP_OK;
            }

            ESP_LOGW(TAG,
                     "deferred diag result: addr=0x%02X pass=%d err=%s probe=0x%02X",
                     address,
                     pass,
                     esp_err_to_name(safe_probe_err),
                     safe_probe_chip_id);
        }

        close_i2c_device(diag_handle);
        ESP_LOGW(TAG, "deferred diag close: addr=0x%02X", address);
    }

    ESP_LOGW(TAG, "deferred diag done: result=%s", esp_err_to_name(final_err));
    return final_err;
}

esp_err_t qmi8658_run_minimal_bringup(void)
{
    uint8_t address = 0;
    uint8_t detect_chip_id = 0;
    uint8_t revision_id = 0;
    uint8_t whoami_after = 0;
    uint8_t revision_after = 0;
    uint8_t ctrl7_after = 0;
    uint8_t ctrl6_after = 0;

    // #region debug-point qmi-min-bringup:entry
    ESP_LOGW(TAG, "minimal bring-up start: settle=%luus", (unsigned long)QMI8658_POWER_ON_SETTLE_US);
    // #endregion debug-point qmi-min-bringup:entry
    s_min_bringup_phase = QMI_MIN_PHASE_SETTLE;
    esp_rom_delay_us(QMI8658_POWER_ON_SETTLE_US);

    // #region debug-point qmi-min-bringup:detect
    ESP_LOGW(TAG, "minimal bring-up detect: probing qmi8658 address list");
    // #endregion debug-point qmi-min-bringup:detect
    s_min_bringup_phase = QMI_MIN_PHASE_DETECT_RETRY;
    const esp_err_t detect_err = detect_qmi8658_address(&address, &detect_chip_id);
    ESP_LOGW(TAG,
             "minimal bring-up detect result: err=%s addr=0x%02X chip=0x%02X",
             esp_err_to_name(detect_err),
             address,
             detect_chip_id);
    if (detect_err != ESP_OK) {
        return detect_err;
    }

    // #region debug-point qmi-min-bringup:revision-id
    ESP_LOGW(TAG,
             "minimal bring-up read revision: addr=0x%02X reg=0x%02X timeout=%dms",
             address,
             QMI8658_REG_REVISION_ID,
             QMI8658_SAFE_PROBE_TIMEOUT_MS);
    // #endregion debug-point qmi-min-bringup:revision-id
    s_min_bringup_phase = QMI_MIN_PHASE_READ_REVISION;
    const esp_err_t revision_err =
        probe_read_register_at_address_with_timeout(address,
                                                    QMI8658_REG_REVISION_ID,
                                                    &revision_id,
                                                    QMI8658_SAFE_PROBE_TIMEOUT_MS);
    ESP_LOGW(TAG,
             "minimal bring-up read revision result: err=%s revision=0x%02X",
             esp_err_to_name(revision_err),
             revision_id);
    if (revision_err != ESP_OK) {
        return revision_err;
    }

    // #region debug-point qmi-min-bringup:ctrl7
    ESP_LOGW(TAG,
             "minimal bring-up write ctrl7: addr=0x%02X reg=0x%02X value=0x43 timeout=%dms",
             address,
             QMI8658_REG_CTRL7,
             QMI8658_SAFE_PROBE_TIMEOUT_MS);
    // #endregion debug-point qmi-min-bringup:ctrl7
    s_min_bringup_phase = QMI_MIN_PHASE_WRITE_CTRL7;
    const esp_err_t ctrl7_err =
        write_register_at_address_with_timeout(address,
                                               QMI8658_REG_CTRL7,
                                               0x43,
                                               QMI8658_SAFE_PROBE_TIMEOUT_MS);
    ESP_LOGW(TAG, "minimal bring-up write ctrl7 result: err=%s", esp_err_to_name(ctrl7_err));
    if (ctrl7_err != ESP_OK) {
        return ctrl7_err;
    }

    // #region debug-point qmi-min-bringup:ctrl6
    ESP_LOGW(TAG,
             "minimal bring-up write ctrl6: addr=0x%02X reg=0x%02X value=0x00 timeout=%dms",
             address,
             QMI8658_REG_CTRL6,
             QMI8658_SAFE_PROBE_TIMEOUT_MS);
    // #endregion debug-point qmi-min-bringup:ctrl6
    s_min_bringup_phase = QMI_MIN_PHASE_WRITE_CTRL6;
    const esp_err_t ctrl6_err =
        write_register_at_address_with_timeout(address,
                                               QMI8658_REG_CTRL6,
                                               0x00,
                                               QMI8658_SAFE_PROBE_TIMEOUT_MS);
    ESP_LOGW(TAG, "minimal bring-up write ctrl6 result: err=%s", esp_err_to_name(ctrl6_err));
    if (ctrl6_err != ESP_OK) {
        return ctrl6_err;
    }

    ESP_LOGW(TAG,
             "minimal bring-up readback whoami: addr=0x%02X reg=0x%02X timeout=%dms",
             address,
             QMI8658_REG_WHO_AM_I,
             QMI8658_SAFE_PROBE_TIMEOUT_MS);
    s_min_bringup_phase = QMI_MIN_PHASE_READBACK_WHOAMI;
    const esp_err_t whoami_after_err =
        probe_read_register_at_address_with_timeout(address,
                                                    QMI8658_REG_WHO_AM_I,
                                                    &whoami_after,
                                                    QMI8658_SAFE_PROBE_TIMEOUT_MS);
    ESP_LOGW(TAG,
             "minimal bring-up readback revision: addr=0x%02X reg=0x%02X timeout=%dms",
             address,
             QMI8658_REG_REVISION_ID,
             QMI8658_SAFE_PROBE_TIMEOUT_MS);
    s_min_bringup_phase = QMI_MIN_PHASE_READBACK_REVISION;
    const esp_err_t revision_after_err =
        probe_read_register_at_address_with_timeout(address,
                                                    QMI8658_REG_REVISION_ID,
                                                    &revision_after,
                                                    QMI8658_SAFE_PROBE_TIMEOUT_MS);
    ESP_LOGW(TAG,
             "minimal bring-up readback ctrl7: addr=0x%02X reg=0x%02X timeout=%dms",
             address,
             QMI8658_REG_CTRL7,
             QMI8658_SAFE_PROBE_TIMEOUT_MS);
    s_min_bringup_phase = QMI_MIN_PHASE_READBACK_CTRL7;
    const esp_err_t ctrl7_after_err =
        probe_read_register_at_address_with_timeout(address,
                                                    QMI8658_REG_CTRL7,
                                                    &ctrl7_after,
                                                    QMI8658_SAFE_PROBE_TIMEOUT_MS);
    ESP_LOGW(TAG,
             "minimal bring-up readback ctrl6: addr=0x%02X reg=0x%02X timeout=%dms",
             address,
             QMI8658_REG_CTRL6,
             QMI8658_SAFE_PROBE_TIMEOUT_MS);
    s_min_bringup_phase = QMI_MIN_PHASE_READBACK_CTRL6;
    const esp_err_t ctrl6_after_read_err =
        probe_read_register_at_address_with_timeout(address,
                                                    QMI8658_REG_CTRL6,
                                                    &ctrl6_after,
                                                    QMI8658_SAFE_PROBE_TIMEOUT_MS);
    ESP_LOGW(TAG,
             "minimal bring-up readback: whoami=%s/0x%02X revision=%s/0x%02X ctrl7=%s/0x%02X ctrl6=%s/0x%02X",
             esp_err_to_name(whoami_after_err),
             whoami_after,
             esp_err_to_name(revision_after_err),
             revision_after,
             esp_err_to_name(ctrl7_after_err),
             ctrl7_after,
             esp_err_to_name(ctrl6_after_read_err),
             ctrl6_after);

    ESP_LOGW(TAG,
             "minimal bring-up official sequence completed: addr=0x%02X detect=0x%02X revision_before=0x%02X",
             address,
             detect_chip_id,
             revision_id);
    s_min_bringup_phase = QMI_MIN_PHASE_DONE;
    return ESP_OK;
}

int qmi8658_get_min_bringup_phase(void)
{
    return (int)s_min_bringup_phase;
}

bool qmi8658_min_bringup_in_progress(void)
{
    return s_min_bringup_phase != QMI_MIN_PHASE_NONE && s_min_bringup_phase != QMI_MIN_PHASE_DONE;
}

const char *qmi8658_get_min_bringup_phase_name(int phase)
{
    switch (phase) {
        case QMI_MIN_PHASE_NONE: return "none";
        case QMI_MIN_PHASE_SETTLE: return "settle";
        case QMI_MIN_PHASE_DETECT_RETRY: return "detect-retry";
        case QMI_MIN_PHASE_DETECT_DUMP: return "detect-dump";
        case QMI_MIN_PHASE_READ_REVISION: return "read-revision";
        case QMI_MIN_PHASE_WRITE_CTRL7: return "write-ctrl7";
        case QMI_MIN_PHASE_WRITE_CTRL6: return "write-ctrl6";
        case QMI_MIN_PHASE_READBACK_WHOAMI: return "readback-whoami";
        case QMI_MIN_PHASE_READBACK_REVISION: return "readback-revision";
        case QMI_MIN_PHASE_READBACK_CTRL7: return "readback-ctrl7";
        case QMI_MIN_PHASE_READBACK_CTRL6: return "readback-ctrl6";
        case QMI_MIN_PHASE_DONE: return "done";
        default: return "unknown";
    }
}

esp_err_t qmi8658_init(void)
{
    uint8_t chip_id = 0;
    uint8_t safe_probe_chip_id = 0;

    if (s_ready) {
        return ESP_OK;
    }

    // Give the IMU a little extra board-level settle time after shared I2C and EXIO come up.
    ESP_LOGI(TAG, "init step: settle");
    esp_rom_delay_us(QMI8658_POWER_ON_SETTLE_US);
    ESP_LOGI(TAG, "init step: detect");
    ESP_RETURN_ON_ERROR(detect_qmi8658_address(&s_i2c_addr, &chip_id), TAG, "detect QMI8658 address failed");

    // Keep normal boot on the safest possible path: only re-check WHO_AM_I with a short timeout.
    ESP_LOGI(TAG, "init step: safe whoami probe timeout=%dms", QMI8658_SAFE_PROBE_TIMEOUT_MS);
    ESP_RETURN_ON_ERROR(probe_read_register_at_address_with_timeout(s_i2c_addr,
                                                                    QMI8658_REG_WHO_AM_I,
                                                                    &safe_probe_chip_id,
                                                                    QMI8658_SAFE_PROBE_TIMEOUT_MS),
                        TAG,
                        "safe WHO_AM_I probe failed");
    ESP_LOGI(TAG, "safe WHO_AM_I addr=0x%02X detect=0x%02X probe=0x%02X", s_i2c_addr, chip_id, safe_probe_chip_id);

    if (safe_probe_chip_id != QMI8658_WHO_AM_I_VALUE) {
        ESP_LOGW(TAG,
                 "IMU WHO_AM_I mismatch on safe path addr=0x%02X detect=0x%02X probe=0x%02X",
                 s_i2c_addr,
                 chip_id,
                 safe_probe_chip_id);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Official vendor init sequence (QMI8658_Init + setState(sensor_running)):
    // CTRL1: enable auto address increment for block reads
    ESP_LOGI(TAG, "init step: ctrl1 aai");
    ESP_RETURN_ON_ERROR(write_register_at_address_with_timeout(s_i2c_addr,
                                                               QMI8658_REG_CTRL1,
                                                               0x40,
                                                               QMI8658_SAFE_PROBE_TIMEOUT_MS),
                        TAG,
                        "write CTRL1 failed");

    // CTRL2: accel 4G range (scale bits 01 << 4)
    ESP_LOGI(TAG, "init step: ctrl2 acc 4g");
    ESP_RETURN_ON_ERROR(write_register_at_address_with_timeout(s_i2c_addr,
                                                               QMI8658_REG_CTRL2,
                                                               0x40,
                                                               QMI8658_SAFE_PROBE_TIMEOUT_MS),
                        TAG,
                        "write CTRL2 failed");

    // CTRL3: gyro 512DPS range (scale bits 011 << 4)
    ESP_LOGI(TAG, "init step: ctrl3 gyro 512dps");
    ESP_RETURN_ON_ERROR(write_register_at_address_with_timeout(s_i2c_addr,
                                                               QMI8658_REG_CTRL3,
                                                               0x30,
                                                               QMI8658_SAFE_PROBE_TIMEOUT_MS),
                        TAG,
                        "write CTRL3 failed");

    // CTRL5: acc LPF mode 0 + enable, gyro LPF mode 3 + enable
    ESP_LOGI(TAG, "init step: ctrl5 lpf");
    ESP_RETURN_ON_ERROR(write_register_at_address_with_timeout(s_i2c_addr,
                                                               QMI8658_REG_CTRL5,
                                                               0x61,
                                                               QMI8658_SAFE_PROBE_TIMEOUT_MS),
                        TAG,
                        "write CTRL5 failed");

    // CTRL7: high-speed internal clock, acc+gyro enabled full mode
    ESP_LOGI(TAG, "init step: ctrl7 enable sensors");
    ESP_RETURN_ON_ERROR(write_register_at_address_with_timeout(s_i2c_addr,
                                                               QMI8658_REG_CTRL7,
                                                               0x43,
                                                               QMI8658_SAFE_PROBE_TIMEOUT_MS),
                        TAG,
                        "write CTRL7 failed");

    // CTRL6: disable AttitudeEngine motion on demand
    ESP_LOGI(TAG, "init step: ctrl6 ae mod off");
    ESP_RETURN_ON_ERROR(write_register_at_address_with_timeout(s_i2c_addr,
                                                               QMI8658_REG_CTRL6,
                                                               0x00,
                                                               QMI8658_SAFE_PROBE_TIMEOUT_MS),
                        TAG,
                        "write CTRL6 failed");

    s_ready = true;
    ESP_LOGI(TAG, "QMI8658 init complete addr=0x%02X chip=0x%02X", s_i2c_addr, chip_id);
    return ESP_OK;
}

bool qmi8658_is_ready(void)
{
    return s_ready;
}

esp_err_t qmi8658_read_sample(qmi8658_sample_t *out_sample)
{
    uint8_t raw_data[QMI8658_READ_LEN] = {0};

    if (out_sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(read_block(QMI8658_REG_TEMP_L, raw_data, sizeof(raw_data)), TAG, "read sample block failed");

    memset(out_sample, 0, sizeof(*out_sample));
    out_sample->raw_temp = decode_i16(&raw_data[0]);
    out_sample->raw_accel_x = decode_i16(&raw_data[2]);
    out_sample->raw_accel_y = decode_i16(&raw_data[4]);
    out_sample->raw_accel_z = decode_i16(&raw_data[6]);
    out_sample->raw_gyro_x = decode_i16(&raw_data[8]);
    out_sample->raw_gyro_y = decode_i16(&raw_data[10]);
    out_sample->raw_gyro_z = decode_i16(&raw_data[12]);

    out_sample->temperature_c = ((float)out_sample->raw_temp / 256.0f) + 25.0f;
    out_sample->accel_x_g = (float)out_sample->raw_accel_x * (4.0f / 32768.0f);
    out_sample->accel_y_g = (float)out_sample->raw_accel_y * (4.0f / 32768.0f);
    out_sample->accel_z_g = (float)out_sample->raw_accel_z * (4.0f / 32768.0f);
    out_sample->gyro_x_dps = (float)out_sample->raw_gyro_x * (512.0f / 32768.0f);
    out_sample->gyro_y_dps = (float)out_sample->raw_gyro_y * (512.0f / 32768.0f);
    out_sample->gyro_z_dps = (float)out_sample->raw_gyro_z * (512.0f / 32768.0f);

    return ESP_OK;
}
