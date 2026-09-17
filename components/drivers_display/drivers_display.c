#include "drivers_display.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "board_support.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_spd2010.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_spd2010.h"
#include "esp_lvgl_port.h"

#include "app_status.h"

static const char *TAG = "drivers_display";

enum {
    LCD_HOST = SPI2_HOST,
    LCD_H_RES = 412,
    LCD_V_RES = 412,
    LCD_DRAW_BUF_LINES = 32,
    LCD_CANVAS_PIXELS = LCD_H_RES * LCD_V_RES,
    LCD_TRANS_PIXELS = LCD_H_RES * LCD_DRAW_BUF_LINES,
    LCD_TOUCH_MAX_POINTS = 1,
};

static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_touch_io;
static esp_lcd_touch_handle_t s_touch;
static lv_disp_t *s_lv_disp;
static bool s_ready;
static bool s_touch_available;
static volatile bool s_touch_irq_pending;
static uint8_t s_touch_poll_skip_count;
// #region debug-point touch:read-health
// Turn "silence after the boot-window NACKs" into positive evidence: count
// read_data() outcomes and report periodically so we can tell "touch works"
// from "touch is never read". (debug-spd2010-touch.md H1/H2)
static uint32_t s_touch_reads_ok;
static uint32_t s_touch_reads_fail;
static uint32_t s_touch_read_seq;
static uint32_t s_touch_pressed_count;
// #endregion

static void IRAM_ATTR touch_isr_handler(void *arg)
{
    (void)arg;
    s_touch_irq_pending = true;
}

static esp_err_t configure_touch_interrupt(void)
{
    const board_pins_t *pins = board_support_get_pins();
    const gpio_config_t touch_int_cfg = {
        .pin_bit_mask = 1ULL << pins->touch_int_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&touch_int_cfg), TAG, "configure touch interrupt failed");

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(pins->touch_int_gpio, touch_isr_handler, NULL),
                        TAG,
                        "install touch ISR failed");
    return ESP_OK;
}

static esp_err_t init_panel_bus(void)
{
    const board_pins_t *pins = board_support_get_pins();
    const spi_bus_config_t buscfg = SPD2010_PANEL_BUS_QSPI_CONFIG(pins->lcd_sck_gpio,
                                                                  pins->lcd_sda0_gpio,
                                                                  pins->lcd_sda1_gpio,
                                                                  pins->lcd_sda2_gpio,
                                                                  pins->lcd_sda3_gpio,
                                                                  LCD_H_RES * LCD_DRAW_BUF_LINES * sizeof(uint16_t));

    esp_err_t err = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    const esp_lcd_panel_io_spi_config_t io_config =
        SPD2010_PANEL_IO_QSPI_CONFIG(pins->lcd_cs_gpio, NULL, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                                 &io_config,
                                                 &s_panel_io),
                        TAG,
                        "create LCD panel IO failed");

    spd2010_vendor_config_t vendor_config = {
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_spd2010(s_panel_io, &panel_config, &s_panel),
                        TAG,
                        "create LCD panel failed");

    return ESP_OK;
}

static esp_err_t init_touch_panel(void)
{
    const board_pins_t *pins = board_support_get_pins();
    i2c_master_bus_handle_t touch_bus = board_support_get_i2c_bus();
    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_SPD2010_CONFIG();

    ESP_RETURN_ON_FALSE(touch_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "touch I2C bus not ready");
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(touch_bus,
                                                 &touch_io_config,
                                                 &s_touch_io),
                        TAG,
                        "create touch IO failed");

    const esp_lcd_touch_config_t touch_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = pins->touch_int_gpio,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_spd2010(s_touch_io, &touch_cfg, &s_touch),
                        TAG,
                        "create touch driver failed");

    // #region debug-point touch:int-pullup
    // configure_touch_interrupt() enabled the pull-up on the INT pin, but the
    // touch component's own gpio_config() during creation does not set
    // pull_up_en and silently clears it. The line then floats LOW, so
    // gpio_get_level() reads 0 forever: our poll degenerates to every-cycle
    // reads and LVGL's EVENT-mode indev never sees a falling edge (widget
    // touch dead). Re-assert the pull-up after component init.
    // (debug-spd2010-touch.md H6)
    gpio_set_pull_mode((gpio_num_t)pins->touch_int_gpio, GPIO_PULLUP_ONLY);
    ESP_LOGI(TAG, "touch int level after pull-up restore=%d", gpio_get_level(pins->touch_int_gpio));
    // #endregion
    return ESP_OK;
}

static esp_err_t init_lvgl_display(void)
{
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "init LVGL port failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_panel_io,
        .panel_handle = s_panel,
        // Keep the main canvas in PSRAM and use a smaller SRAM transfer buffer for QSPI flushes.
        .buffer_size = LCD_CANVAS_PIXELS,
        .trans_size = LCD_TRANS_PIXELS,
        .double_buffer = false,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = false,
        },
    };

    s_lv_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_lv_disp == NULL) {
        return ESP_FAIL;
    }

    if (s_touch != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = s_lv_disp,
            .handle = s_touch,
        };
        if (lvgl_port_add_touch(&touch_cfg) == NULL) {
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

esp_err_t drivers_display_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    const esp_err_t touch_irq_err = configure_touch_interrupt();
    if (touch_irq_err != ESP_OK) {
        APP_LOGE(TAG, APP_ERR_TOUCH_INIT, "touch interrupt setup failed: %s", esp_err_to_name(touch_irq_err));
        return touch_irq_err;
    }

    ESP_RETURN_ON_ERROR(board_support_set_lcd_backlight(false), TAG, "disable backlight failed");
    ESP_RETURN_ON_ERROR(board_support_reset_lcd(), TAG, "reset LCD failed");
    ESP_RETURN_ON_ERROR(board_support_reset_touch(), TAG, "reset touch failed");

    const esp_err_t lcd_err = init_panel_bus();
    if (lcd_err != ESP_OK) {
        APP_LOGE(TAG, APP_ERR_LCD_INIT, "LCD bring-up failed: %s", esp_err_to_name(lcd_err));
        return lcd_err;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset callback failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel on failed");
    ESP_RETURN_ON_ERROR(board_support_set_lcd_backlight(true), TAG, "enable backlight failed");

    const esp_err_t touch_err = init_touch_panel();
    if (touch_err != ESP_OK) {
        s_touch = NULL;
        s_touch_available = false;
        APP_LOGW(TAG, APP_ERR_TOUCH_INIT, "touch bring-up skipped: %s", esp_err_to_name(touch_err));
    } else {
        s_touch_available = true;
    }

    const esp_err_t lvgl_err = init_lvgl_display();
    if (lvgl_err != ESP_OK) {
        APP_LOGE(TAG, APP_ERR_LVGL_INIT, "LVGL bring-up failed: %s", esp_err_to_name(lvgl_err));
        return lvgl_err;
    }

    s_touch_irq_pending = false;
    s_touch_poll_skip_count = 0;
    s_ready = true;
    APP_LOGI(TAG,
             APP_STATUS_OK,
             "LCD and LVGL ready touch=%s",
             s_touch_available ? "enabled" : "disabled");
    return ESP_OK;
}

bool drivers_display_is_ready(void)
{
    return s_ready;
}

bool drivers_display_lock(uint32_t timeout_ms)
{
    if (!s_ready) {
        return false;
    }

    lvgl_port_lock(timeout_ms);
    return true;
}

void drivers_display_unlock(void)
{
    if (s_ready) {
        lvgl_port_unlock();
    }
}

esp_err_t drivers_display_poll_touch(drivers_display_touch_sample_t *out_sample)
{
    const board_pins_t *pins = board_support_get_pins();
    bool should_poll = false;

    if (out_sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(out_sample, 0, sizeof(*out_sample));
    if (s_touch == NULL) {
        return ESP_OK;
    }

    should_poll = s_touch_irq_pending || gpio_get_level(pins->touch_int_gpio) == 0;
    if (!should_poll) {
        if (++s_touch_poll_skip_count < 4) {
            return ESP_OK;
        }
        s_touch_poll_skip_count = 0;
    }

    out_sample->irq_triggered = should_poll;

    // #region debug-point touch:read-health
    {
        const esp_err_t read_err = esp_lcd_touch_read_data(s_touch);
        ++s_touch_read_seq;
        if (read_err == ESP_OK) {
            ++s_touch_reads_ok;
        } else {
            ++s_touch_reads_fail;
        }
        if ((s_touch_read_seq % 128U) == 0U) {
            // INT level sampled here: 0 = asserted (active-low). If it reads 0
            // forever, the line is stuck low and LVGL's EVENT-mode indev never
            // fires (no edges) — widget-level touch would be dead even though
            // the poll path works. (debug-spd2010-touch.md H2 follow-up)
            ESP_LOGI(TAG,
                     "touch health reads=%" PRIu32 " ok=%" PRIu32 " fail=%" PRIu32 " pressed=%" PRIu32 " int=%d",
                     s_touch_read_seq,
                     s_touch_reads_ok,
                     s_touch_reads_fail,
                     s_touch_pressed_count,
                     gpio_get_level(board_support_get_pins()->touch_int_gpio));
        }
    }
    // #endregion

    esp_lcd_touch_point_data_t points[LCD_TOUCH_MAX_POINTS] = {0};
    uint8_t count = 0;
    (void)esp_lcd_touch_get_data(s_touch, points, &count, LCD_TOUCH_MAX_POINTS);

    out_sample->count = count;
    out_sample->pressed = count > 0;
    if (count > 0) {
        ++s_touch_pressed_count;
        out_sample->x = points[0].x;
        out_sample->y = points[0].y;
        out_sample->strength = points[0].strength;
    }

    s_touch_irq_pending = false;
    return ESP_OK;
}

lv_disp_t *drivers_display_get_lv_disp(void)
{
    return s_lv_disp;
}
