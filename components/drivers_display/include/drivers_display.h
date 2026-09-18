#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

typedef struct {
    bool pressed;
    bool irq_triggered;
    /* true = 本次调用未读硬件 (中断未触发且处于节流跳读周期), 字段无意义 */
    bool skipped;
    uint16_t x;
    uint16_t y;
    uint16_t strength;
    uint8_t count;
} drivers_display_touch_sample_t;

esp_err_t drivers_display_init(void);
bool drivers_display_is_ready(void);
bool drivers_display_lock(uint32_t timeout_ms);
void drivers_display_unlock(void);
esp_err_t drivers_display_poll_touch(drivers_display_touch_sample_t *out_sample);
lv_display_t *drivers_display_get_lv_disp(void);
