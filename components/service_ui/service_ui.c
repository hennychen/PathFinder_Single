#include "service_ui.h"

#include <stdlib.h>
#include <string.h>

#include "app_pages.h"
#include "app_state.h"
#include "app_status.h"
#include "board_support.h"
#include "drivers_display.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "service_attitude.h"
#include "service_voice.h"
#include "ui_pages.h"
#include "ui_render.h"
#include "ui_theme.h"

static const char *TAG = "service_ui";

enum {
    BOOT_KEY_LONG_PRESS_TICKS = pdMS_TO_TICKS(2000), /* 长按阈值 2s: 姿态页触发校准 */
};

typedef struct {
    bool tracking;
    uint8_t release_count; /* 连续未按下计数, 去抖防误判松手 */
    uint16_t start_x;
    uint16_t start_y;
    uint16_t last_x;
    uint16_t last_y;
} gesture_state_t;

static lv_obj_t *s_screen;
static ui_widgets_t s_widgets;
static TaskHandle_t s_ui_task;

static uint32_t get_page_refresh_period_ms(app_page_id_t page)
{
    switch (page) {
    case PAGE_ATTITUDE:
        return 50;
    case PAGE_COMPASS:
        return 100;
    case PAGE_INCLINE:
        return 50;
    case PAGE_NAV:
        return 200;
    case PAGE_OBD:
        return 200;
    case PAGE_VOICE:
        return 100;
    case PAGE_SYSTEM:
    default:
        return 500;
    }
}

static bool is_page_switch_locked(void)
{
    const ui_page_request_t request = app_state_get_page_request();
    return request.force_page_active;
}

static void render_current_page_locked(void)
{
    workflow_state_t state = {0};
    app_state_get_snapshot(&state);
    ui_render_page(state.current_page, &state, &s_widgets);
}

static void step_to_page(app_page_id_t target_page)
{
    if (is_page_switch_locked()) {
        return;
    }

    service_ui_show_page(target_page);
}

/* 姿态页长按 BOOT 键 2s: 触发安装基准校准 (设备按实际安装姿态静止放置) */
static void run_attitude_calibration(void)
{
    APP_LOGI(TAG, APP_EVT_ATTITUDE_CALIB, "attitude mount calibration requested");
    app_state_set_attitude_calibrating(true);
    service_ui_render_active_page();

    const esp_err_t err = service_attitude_start_calibration();

    app_state_set_attitude_calibrating(false);
    if (err == ESP_OK) {
        APP_LOGI(TAG, APP_EVT_ATTITUDE_CALIB, "attitude mount calibration succeeded");
    } else {
        APP_LOGW(TAG, APP_EVT_ATTITUDE_CALIB, "attitude mount calibration failed: %s", esp_err_to_name(err));
    }
    service_ui_render_active_page();
}

static void process_boot_key(bool *last_pressed, TickType_t *press_start, bool *long_press_fired)
{
    const bool pressed = board_support_is_boot_key_pressed();

    if (pressed && !(*last_pressed)) {
        app_state_mark_user_activity();
        *press_start = xTaskGetTickCount();
        *long_press_fired = false;
    }

    if (pressed && *last_pressed && !(*long_press_fired)
        && (xTaskGetTickCount() - *press_start) >= BOOT_KEY_LONG_PRESS_TICKS) {
        *long_press_fired = true;
        workflow_state_t state = {0};
        app_state_get_snapshot(&state);
        if (state.current_page == PAGE_ATTITUDE) {
            run_attitude_calibration();
        } else {
            APP_LOGI(TAG, APP_EVT_BOOT_KEY, "long press ignored on page=%s (only ATTITUDE calibrates)",
                     app_pages_to_string(state.current_page));
        }
    }

    /* 短按: 松手时且未触发长按才切页 */
    if (!pressed && *last_pressed) {
        if (!(*long_press_fired)) {
            workflow_state_t state = {0};
            app_state_get_snapshot(&state);
            APP_LOGI(TAG,
                     APP_EVT_BOOT_KEY,
                     "BOOT key pressed on page=%s",
                     app_pages_to_string(state.current_page));
            step_to_page(app_pages_next(state.current_page));
        }
    }

    *last_pressed = pressed;
}

static void process_touch_gesture(gesture_state_t *gesture)
{
    drivers_display_touch_sample_t sample = {0};
    if (drivers_display_poll_touch(&sample) != ESP_OK) {
        return;
    }

    /* 节流跳读周期: 本次没有真实硬件数据, 保持当前手势状态不变 */
    if (sample.skipped) {
        return;
    }

    if (sample.pressed && !gesture->tracking) {
        gesture->tracking = true;
        gesture->release_count = 0;
        gesture->start_x = sample.x;
        gesture->start_y = sample.y;
        gesture->last_x = sample.x;
        gesture->last_y = sample.y;
        app_state_mark_user_activity();
        APP_LOGI(TAG,
                 APP_EVT_TOUCH,
                 "touch down x=%u y=%u strength=%u irq=%d",
                 sample.x,
                 sample.y,
                 sample.strength,
                 sample.irq_triggered);
        return;
    }

    if (sample.pressed && gesture->tracking) {
        gesture->release_count = 0;
        gesture->last_x = sample.x;
        gesture->last_y = sample.y;
        return;
    }

    if (!sample.pressed && gesture->tracking) {
        /* 松手去抖: 连续 2 次采样未按下才结算, 防止单次误读中断滑动 */
        if (++gesture->release_count < 2) {
            return;
        }

        const int dx = (int)gesture->last_x - (int)gesture->start_x;
        const int dy = (int)gesture->last_y - (int)gesture->start_y;

        APP_LOGI(TAG,
                 APP_EVT_TOUCH,
                 "touch up start=(%u,%u) end=(%u,%u) dx=%d dy=%d",
                 gesture->start_x,
                 gesture->start_y,
                 gesture->last_x,
                 gesture->last_y,
                 dx,
                 dy);

        if (dx >= 40 && dx > 0 && abs(dx) > abs(dy)) {
            workflow_state_t state = {0};
            app_state_get_snapshot(&state);
            step_to_page(app_pages_prev(state.current_page));
        } else if (dx <= -40 && abs(dx) > abs(dy)) {
            workflow_state_t state = {0};
            app_state_get_snapshot(&state);
            step_to_page(app_pages_next(state.current_page));
        }

        memset(gesture, 0, sizeof(*gesture));
    }
}

static void task_ui_runtime(void *arg)
{
    (void)arg;

    bool last_boot_pressed = false;
    TickType_t boot_press_start = 0;
    bool boot_long_press_fired = false;
    gesture_state_t gesture = {0};
    TickType_t last_render_tick = 0;

    while (true) {
        workflow_state_t state = {0};

        process_boot_key(&last_boot_pressed, &boot_press_start, &boot_long_press_fired);
        process_touch_gesture(&gesture);
        app_state_get_snapshot(&state);

        const TickType_t now = xTaskGetTickCount();
        const TickType_t refresh_period = pdMS_TO_TICKS(get_page_refresh_period_ms(state.current_page));
        if (now - last_render_tick >= refresh_period) {
            service_ui_render_active_page();
            last_render_tick = now;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t service_ui_init(void)
{
    ESP_RETURN_ON_ERROR(drivers_display_init(), TAG, "display stack init failed");

    if (!drivers_display_lock(0)) {
        return ESP_ERR_TIMEOUT;
    }

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(PF_CLR_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_radius(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    esp_err_t build_err = ui_pages_build(s_screen, &s_widgets);
    if (build_err == ESP_OK) {
        render_current_page_locked();
        lv_screen_load(s_screen);
    }
    drivers_display_unlock();

    if (build_err != ESP_OK) {
        APP_LOGE(TAG, APP_ERR_UI_INIT, "build UI pages failed: %s", esp_err_to_name(build_err));
        return build_err;
    }

    app_state_set_ui_ready(true);
    if (s_ui_task == NULL) {
        xTaskCreatePinnedToCore(task_ui_runtime, "task_ui_runtime", 6144, NULL, 4, &s_ui_task, 1);
    }

    APP_LOGI(TAG, APP_STATUS_OK, "UI service initialized with LVGL");
    service_ui_show_page(PAGE_SYSTEM);
    return ESP_OK;
}

void service_ui_show_page(app_page_id_t page)
{
    if (is_page_switch_locked() && page != PAGE_VOICE) {
        return;
    }

    app_state_set_user_page(page);
    service_ui_render_active_page();
}

void service_ui_render_active_page(void)
{
    if (!drivers_display_is_ready()) {
        return;
    }

    if (!drivers_display_lock(0)) {
        return;
    }

    render_current_page_locked();
    drivers_display_unlock();
}
