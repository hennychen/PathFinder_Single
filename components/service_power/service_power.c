#include "service_power.h"

#include "app_state.h"
#include "app_status.h"
#include "board_support.h"
#include "drivers_power.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "service_power";

enum {
    POWER_TASK_STACK = 3072,
    POWER_TASK_PRIORITY = 1,
    POWER_TASK_CORE = 0,
    POWER_TASK_PERIOD_MS = 500,
    POWER_BATTERY_SAMPLE_MS = 2000,
    // Keep the screen on during board bring-up so display validation isn't masked by auto-dim.
    POWER_DIM_BACKLIGHT_MS = 30 * 60 * 1000,
    POWER_LIGHT_SLEEP_READY_MS = 60 * 60 * 1000,
};

static bool s_ready;
static TaskHandle_t s_task_handle;
static bool s_backlight_dimmed;
static uint32_t s_last_battery_sample_ms;
static bool s_sleep_wakeup_armed;

static uint32_t power_now_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void publish_power_state(uint32_t idle_duration_ms)
{
    power_runtime_state_t state = {
        .backlight_dimmed = s_backlight_dimmed,
        .light_sleep_ready = idle_duration_ms >= POWER_LIGHT_SLEEP_READY_MS,
        .idle_duration_ms = idle_duration_ms,
    };

    app_state_set_power_state(&state);
}

static esp_err_t prepare_light_sleep_wakeup(void)
{
    const board_pins_t *pins = board_support_get_pins();

    if (s_sleep_wakeup_armed) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(gpio_wakeup_enable((gpio_num_t)pins->boot_key_gpio, GPIO_INTR_LOW_LEVEL),
                        TAG,
                        "enable boot wakeup failed");
    ESP_RETURN_ON_ERROR(gpio_wakeup_enable((gpio_num_t)pins->touch_int_gpio, GPIO_INTR_LOW_LEVEL),
                        TAG,
                        "enable touch wakeup failed");
    ESP_RETURN_ON_ERROR(esp_sleep_enable_gpio_wakeup(), TAG, "enable gpio light sleep wakeup failed");

    s_sleep_wakeup_armed = true;
    return ESP_OK;
}

static void enter_light_sleep_if_needed(void)
{
    workflow_state_t state = {0};

    app_state_get_snapshot(&state);
    if (!state.power.light_sleep_ready) {
        return;
    }

    if (prepare_light_sleep_wakeup() != ESP_OK) {
        return;
    }

    APP_LOGI(TAG,
             APP_EVT_POWER_POLICY,
             "enter light sleep idle=%lums page=%s",
             (unsigned long)state.power.idle_duration_ms,
             app_pages_to_string(state.current_page));

    (void)board_support_set_lcd_backlight(false);
    esp_light_sleep_start();

    app_state_mark_user_activity();
    s_backlight_dimmed = false;
    (void)board_support_set_lcd_backlight(true);

    APP_LOGI(TAG,
             APP_EVT_POWER_POLICY,
             "wake from light sleep cause=%d",
             (int)esp_sleep_get_wakeup_cause());
}

static void process_power_policy(void)
{
    workflow_state_t state = {0};
    const uint32_t now_ms = power_now_ms();
    bool keep_awake = false;
    bool should_dim = false;
    uint32_t idle_duration_ms = 0;

    app_state_get_snapshot(&state);
    if (now_ms > state.last_user_action_ms) {
        idle_duration_ms = now_ms - state.last_user_action_ms;
    }

    keep_awake = state.voice_active || state.nav_active || state.obd.alert_active;
    should_dim = !keep_awake && idle_duration_ms >= POWER_DIM_BACKLIGHT_MS;
    if (should_dim != s_backlight_dimmed) {
        if (board_support_set_lcd_backlight(!should_dim) == ESP_OK) {
            s_backlight_dimmed = should_dim;
            APP_LOGI(TAG,
                     APP_EVT_POWER_POLICY,
                     "power policy idle=%lums keep_awake=%d backlight=%s sleep_ready=%d",
                     (unsigned long)idle_duration_ms,
                     keep_awake,
                     s_backlight_dimmed ? "dimmed" : "on",
                     !keep_awake && idle_duration_ms >= POWER_LIGHT_SLEEP_READY_MS);
        }
    }

    if (keep_awake && idle_duration_ms >= POWER_LIGHT_SLEEP_READY_MS) {
        idle_duration_ms = POWER_LIGHT_SLEEP_READY_MS - 1U;
    }

    publish_power_state(idle_duration_ms);
}

static void process_battery_sampling(void)
{
    const uint32_t now_ms = power_now_ms();
    drivers_power_status_t status = {0};

    if ((now_ms - s_last_battery_sample_ms) < POWER_BATTERY_SAMPLE_MS) {
        return;
    }

    if (drivers_power_read_status(&status) != ESP_OK) {
        return;
    }

    s_last_battery_sample_ms = now_ms;
    app_state_set_battery_state(status.battery_percent, status.battery_voltage_mv);
}

static void task_power_runtime(void *arg)
{
    (void)arg;
    s_ready = true;

    while (true) {
        process_power_policy();
        process_battery_sampling();
        enter_light_sleep_if_needed();
        vTaskDelay(pdMS_TO_TICKS(POWER_TASK_PERIOD_MS));
    }
}

esp_err_t service_power_init(void)
{
    if (s_task_handle != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(drivers_power_init(), TAG, "battery adc init failed");
    s_backlight_dimmed = false;
    s_last_battery_sample_ms = 0;
    s_sleep_wakeup_armed = false;
    publish_power_state(0);

    const BaseType_t created = xTaskCreatePinnedToCore(task_power_runtime,
                                                       "task_power",
                                                       POWER_TASK_STACK,
                                                       NULL,
                                                       POWER_TASK_PRIORITY,
                                                       &s_task_handle,
                                                       POWER_TASK_CORE);
    if (created != pdPASS) {
        s_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    APP_LOGI(TAG,
             APP_STATUS_OK,
             "power service ready dim=%dms sleep_ready=%dms core=%d priority=%d",
             POWER_DIM_BACKLIGHT_MS,
             POWER_LIGHT_SLEEP_READY_MS,
             POWER_TASK_CORE,
             POWER_TASK_PRIORITY);
    return ESP_OK;
}

bool service_power_is_ready(void)
{
    return s_ready;
}
