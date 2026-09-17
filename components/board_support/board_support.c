#include "board_support.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "tca9554.h"

#include "app_status.h"

static const char *TAG = "board_support";
static bool s_i2c_ready;
static bool s_boot_gpio_ready;
static bool s_backlight_gpio_ready;
static i2c_master_bus_handle_t s_i2c_bus;

enum {
    BOARD_EXIO_I2C_ADDR = 0x20,
};

static const board_pins_t k_board_pins = {
    .i2c_port = I2C_NUM_0,
    .i2c_scl_gpio = 10,
    .i2c_sda_gpio = 11,
    .lcd_sda0_gpio = 46,
    .lcd_sda1_gpio = 45,
    .lcd_sda2_gpio = 42,
    .lcd_sda3_gpio = 41,
    .lcd_sck_gpio = 40,
    .lcd_cs_gpio = 21,
    .lcd_te_gpio = 18,
    .lcd_bl_gpio = 5,
    .touch_int_gpio = 4,
    .rtc_int_gpio = 9,
    .mic_ws_gpio = 2,
    .mic_sck_gpio = 15,
    .mic_sd_gpio = 39,
    .audio_bclk_gpio = 48,
    .audio_lrc_gpio = 38,
    .audio_dout_gpio = 47,
    .sd_sck_gpio = 14,
    .sd_miso_gpio = 16,
    .sd_mosi_gpio = 17,
    .battery_adc_gpio = 8,
    .boot_key_gpio = 0,
};

static const uint8_t k_touch_reset_exio = 1;
static const uint8_t k_lcd_reset_exio = 2;
static const uint8_t k_sd_cs_exio = 3;
static const uint8_t k_imu_int2_exio = 4;
static const uint8_t k_imu_int1_exio = 5;

const board_pins_t *board_support_get_pins(void)
{
    return &k_board_pins;
}

i2c_master_bus_handle_t board_support_get_i2c_bus(void)
{
    return s_i2c_bus;
}

uint8_t board_support_get_touch_reset_exio(void)
{
    return k_touch_reset_exio;
}

uint8_t board_support_get_lcd_reset_exio(void)
{
    return k_lcd_reset_exio;
}

uint8_t board_support_get_sd_cs_exio(void)
{
    return k_sd_cs_exio;
}

uint8_t board_support_get_imu_int1_exio(void)
{
    return k_imu_int1_exio;
}

uint8_t board_support_get_imu_int2_exio(void)
{
    return k_imu_int2_exio;
}

static esp_err_t configure_gpio_outputs(void)
{
    if (!s_backlight_gpio_ready) {
        const gpio_config_t backlight_cfg = {
            .pin_bit_mask = 1ULL << k_board_pins.lcd_bl_gpio,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&backlight_cfg), TAG, "configure backlight gpio failed");
        ESP_RETURN_ON_ERROR(gpio_set_level(k_board_pins.lcd_bl_gpio, 0), TAG, "default backlight level failed");
        s_backlight_gpio_ready = true;
    }

    if (!s_boot_gpio_ready) {
        const gpio_config_t boot_cfg = {
            .pin_bit_mask = 1ULL << k_board_pins.boot_key_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&boot_cfg), TAG, "configure boot key failed");
        s_boot_gpio_ready = true;
    }

    return ESP_OK;
}

static esp_err_t configure_exio_defaults(void)
{
    ESP_RETURN_ON_ERROR(tca9554_init(s_i2c_bus, BOARD_EXIO_I2C_ADDR),
                        TAG,
                        "probe IO expander failed");

    ESP_RETURN_ON_ERROR(tca9554_set_pin_mode(k_touch_reset_exio, true), TAG, "touch rst mode failed");
    ESP_RETURN_ON_ERROR(tca9554_set_pin_mode(k_lcd_reset_exio, true), TAG, "lcd rst mode failed");
    ESP_RETURN_ON_ERROR(tca9554_set_pin_mode(k_sd_cs_exio, true), TAG, "sd cs mode failed");
    ESP_RETURN_ON_ERROR(tca9554_set_pin_mode(k_imu_int2_exio, false), TAG, "imu int2 mode failed");
    ESP_RETURN_ON_ERROR(tca9554_set_pin_mode(k_imu_int1_exio, false), TAG, "imu int1 mode failed");

    ESP_RETURN_ON_ERROR(tca9554_write_pin(k_touch_reset_exio, true), TAG, "touch rst default failed");
    ESP_RETURN_ON_ERROR(tca9554_write_pin(k_lcd_reset_exio, true), TAG, "lcd rst default failed");
    ESP_RETURN_ON_ERROR(tca9554_write_pin(k_sd_cs_exio, true), TAG, "sd cs default failed");

    return ESP_OK;
}

esp_err_t board_support_reset_lcd(void)
{
    ESP_RETURN_ON_ERROR(tca9554_write_pin(k_lcd_reset_exio, false), TAG, "lcd reset low failed");
    esp_rom_delay_us(20000);
    ESP_RETURN_ON_ERROR(tca9554_write_pin(k_lcd_reset_exio, true), TAG, "lcd reset high failed");
    esp_rom_delay_us(120000);
    return ESP_OK;
}

esp_err_t board_support_reset_touch(void)
{
    ESP_RETURN_ON_ERROR(tca9554_write_pin(k_touch_reset_exio, false), TAG, "touch reset low failed");
    esp_rom_delay_us(20000);
    ESP_RETURN_ON_ERROR(tca9554_write_pin(k_touch_reset_exio, true), TAG, "touch reset high failed");
    esp_rom_delay_us(80000);
    return ESP_OK;
}

esp_err_t board_support_set_lcd_backlight(bool enabled)
{
    if (!s_backlight_gpio_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    return gpio_set_level(k_board_pins.lcd_bl_gpio, enabled ? 1 : 0);
}

esp_err_t board_support_set_sd_card_selected(bool selected)
{
    return tca9554_write_pin(k_sd_cs_exio, !selected);
}

bool board_support_is_boot_key_pressed(void)
{
    if (!s_boot_gpio_ready) {
        return false;
    }

    return gpio_get_level(k_board_pins.boot_key_gpio) == 0;
}

esp_err_t board_support_init(void)
{
    if (!s_i2c_ready) {
        const i2c_master_bus_config_t i2c_config = {
            .i2c_port = k_board_pins.i2c_port,
            .sda_io_num = k_board_pins.i2c_sda_gpio,
            .scl_io_num = k_board_pins.i2c_scl_gpio,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            // #region debug-point qmi-min-bringup:queue-depth-0
            // Was 4 (experimental async trans-queue path, see boot warning);
            // hung 0x6B transactions with idle lines and ignored timeouts.
            // 0 = fully synchronous driver path used by stock examples.
            .trans_queue_depth = 0,
            // #endregion debug-point qmi-min-bringup:queue-depth-0
            .flags = {
                .enable_internal_pullup = 1,
            },
        };

        const esp_err_t bus_err = i2c_new_master_bus(&i2c_config, &s_i2c_bus);
        if (bus_err != ESP_OK && bus_err != ESP_ERR_INVALID_STATE) {
            APP_LOGE(TAG, APP_ERR_BOARD_I2C_INIT, "create I2C bus failed: %s", esp_err_to_name(bus_err));
            return bus_err;
        }
        if (bus_err == ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(i2c_master_get_bus_handle((i2c_port_num_t)k_board_pins.i2c_port, &s_i2c_bus),
                                TAG,
                                "get existing I2C bus failed");
        }

        s_i2c_ready = true;
    }

    const esp_err_t gpio_err = configure_gpio_outputs();
    if (gpio_err != ESP_OK) {
        APP_LOGE(TAG, APP_ERR_BOARD_GPIO_INIT, "configure board GPIO failed: %s", esp_err_to_name(gpio_err));
        return gpio_err;
    }

    const esp_err_t exio_err = configure_exio_defaults();
    if (exio_err != ESP_OK) {
        APP_LOGE(TAG, APP_ERR_BOARD_EXIO_INIT, "configure EXIO failed: %s", esp_err_to_name(exio_err));
        return exio_err;
    }

    APP_LOGI(TAG,
             APP_STATUS_OK,
             "board pins: i2c(%d,%d,%d) lcd(qspi=%d,%d,%d,%d,%d cs=%d te=%d bl=%d rst=EXIO%d) "
             "touch(int=%d,rst=EXIO%d) rtc(int=%d) imu(int1=EXIO%d,int2=EXIO%d) "
             "mic(ws=%d sck=%d sd=%d) speaker(bclk=%d lrc=%d dout=%d) "
             "sd(sck=%d miso=%d mosi=%d cs=EXIO%d) bat_adc=%d boot=%d",
             k_board_pins.i2c_port,
             k_board_pins.i2c_scl_gpio,
             k_board_pins.i2c_sda_gpio,
             k_board_pins.lcd_sda0_gpio,
             k_board_pins.lcd_sda1_gpio,
             k_board_pins.lcd_sda2_gpio,
             k_board_pins.lcd_sda3_gpio,
             k_board_pins.lcd_sck_gpio,
             k_board_pins.lcd_cs_gpio,
             k_board_pins.lcd_te_gpio,
             k_board_pins.lcd_bl_gpio,
             k_lcd_reset_exio,
             k_board_pins.touch_int_gpio,
             k_touch_reset_exio,
             k_board_pins.rtc_int_gpio,
             k_imu_int1_exio,
             k_imu_int2_exio,
             k_board_pins.mic_ws_gpio,
             k_board_pins.mic_sck_gpio,
             k_board_pins.mic_sd_gpio,
             k_board_pins.audio_bclk_gpio,
             k_board_pins.audio_lrc_gpio,
             k_board_pins.audio_dout_gpio,
             k_board_pins.sd_sck_gpio,
             k_board_pins.sd_miso_gpio,
             k_board_pins.sd_mosi_gpio,
             k_sd_cs_exio,
             k_board_pins.battery_adc_gpio,
             k_board_pins.boot_key_gpio);

    return ESP_OK;
}
