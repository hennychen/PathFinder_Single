#include "service_log.h"

#include <stdarg.h>
#include <stdio.h>

#include "app_status.h"
#include "board_support.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

static const char *TAG = "service_log";

static const char *k_mount_point = "/sdcard";
static const char *k_log_path = "/sdcard/pathfinder.log";

static bool s_ready;
static bool s_mounted;
static FILE *s_log_file;
static sdmmc_card_t *s_card;
static vprintf_like_t s_previous_vprintf;
static SemaphoreHandle_t s_write_lock;

static int tee_log_output(const char *format, va_list args)
{
    int written = 0;

    va_list console_args;
    va_copy(console_args, args);
    if (s_previous_vprintf != NULL) {
        written = s_previous_vprintf(format, console_args);
    } else {
        written = vprintf(format, console_args);
    }
    va_end(console_args);

    if (s_log_file == NULL || s_write_lock == NULL) {
        return written;
    }

    if (xSemaphoreTake(s_write_lock, 0) == pdTRUE) {
        va_list file_args;
        va_copy(file_args, args);
        (void)vfprintf(s_log_file, format, file_args);
        va_end(file_args);
        fflush(s_log_file);
        xSemaphoreGive(s_write_lock);
    }

    return written;
}

static esp_err_t mount_sd_card(void)
{
    const board_pins_t *pins = board_support_get_pins();
    const spi_bus_config_t bus_config = {
        .mosi_io_num = pins->sd_mosi_gpio,
        .miso_io_num = pins->sd_miso_gpio,
        .sclk_io_num = pins->sd_sck_gpio,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 4096,
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    host.slot = SPI3_HOST;
    slot_config.host_id = host.slot;
    slot_config.gpio_cs = GPIO_NUM_NC;

    ESP_RETURN_ON_ERROR(board_support_set_sd_card_selected(true), TAG, "select sd card failed");

    esp_err_t err = spi_bus_initialize(host.slot, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        (void)board_support_set_sd_card_selected(false);
        return err;
    }

    err = esp_vfs_fat_sdspi_mount(k_mount_point, &host, &slot_config, &mount_config, &s_card);
    if (err != ESP_OK) {
        (void)board_support_set_sd_card_selected(false);
        (void)spi_bus_free(host.slot);
        return err;
    }

    s_mounted = true;
    return ESP_OK;
}

static void cleanup_log_service(void)
{
    if (s_previous_vprintf != NULL) {
        esp_log_set_vprintf(s_previous_vprintf);
        s_previous_vprintf = NULL;
    }

    if (s_log_file != NULL) {
        fflush(s_log_file);
        fclose(s_log_file);
        s_log_file = NULL;
    }

    if (s_mounted) {
        esp_vfs_fat_sdcard_unmount(k_mount_point, s_card);
        s_card = NULL;
        s_mounted = false;
        (void)spi_bus_free(SPI3_HOST);
    }

    (void)board_support_set_sd_card_selected(false);
    s_ready = false;
}

esp_err_t service_log_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    if (s_write_lock == NULL) {
        s_write_lock = xSemaphoreCreateMutex();
        if (s_write_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = mount_sd_card();
    if (err != ESP_OK) {
        cleanup_log_service();
        return err;
    }

    s_log_file = fopen(k_log_path, "a");
    if (s_log_file == NULL) {
        cleanup_log_service();
        return ESP_FAIL;
    }

    setvbuf(s_log_file, NULL, _IOLBF, 0);
    s_previous_vprintf = esp_log_set_vprintf(tee_log_output);
    s_ready = true;

    APP_LOGI(TAG,
             APP_EVT_LOG_SINK,
             "log sink ready path=%s spi(sck=%d miso=%d mosi=%d cs=EXIO%d)",
             k_log_path,
             board_support_get_pins()->sd_sck_gpio,
             board_support_get_pins()->sd_miso_gpio,
             board_support_get_pins()->sd_mosi_gpio,
             board_support_get_sd_cs_exio());
    return ESP_OK;
}

bool service_log_is_ready(void)
{
    return s_ready;
}

const char *service_log_get_path(void)
{
    return k_log_path;
}
