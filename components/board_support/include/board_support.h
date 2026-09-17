#pragma once

#include "driver/i2c_master.h"
#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int i2c_port;
    int i2c_scl_gpio;
    int i2c_sda_gpio;
    int lcd_sda0_gpio;
    int lcd_sda1_gpio;
    int lcd_sda2_gpio;
    int lcd_sda3_gpio;
    int lcd_sck_gpio;
    int lcd_cs_gpio;
    int lcd_te_gpio;
    int lcd_bl_gpio;
    int touch_int_gpio;
    int rtc_int_gpio;
    int mic_ws_gpio;
    int mic_sck_gpio;
    int mic_sd_gpio;
    int audio_bclk_gpio;
    int audio_lrc_gpio;
    int audio_dout_gpio;
    int sd_sck_gpio;
    int sd_miso_gpio;
    int sd_mosi_gpio;
    int battery_adc_gpio;
    int boot_key_gpio;
} board_pins_t;

const board_pins_t *board_support_get_pins(void);
i2c_master_bus_handle_t board_support_get_i2c_bus(void);
uint8_t board_support_get_touch_reset_exio(void);
uint8_t board_support_get_lcd_reset_exio(void);
uint8_t board_support_get_sd_cs_exio(void);
uint8_t board_support_get_imu_int1_exio(void);
uint8_t board_support_get_imu_int2_exio(void);
esp_err_t board_support_reset_lcd(void);
esp_err_t board_support_reset_touch(void);
esp_err_t board_support_set_lcd_backlight(bool enabled);
esp_err_t board_support_set_sd_card_selected(bool selected);
bool board_support_is_boot_key_pressed(void);
esp_err_t board_support_init(void);
