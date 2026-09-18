#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#include "ui_theme.h"

/*
 * UI 页面构建器：智能手表风格（黑底、大数字、环形仪表、克制用色）
 * 每个页面一个 build_* 函数，全部一次性创建后按需隐藏/显示切换。
 */

typedef struct {
    /* 全局 chrome */
    lv_obj_t *page_dots[8]; /* PAGE_COUNT 最大 8 */

    /* SYSTEM 表盘 */
    lv_obj_t *sys_panel;
    lv_obj_t *sys_time_label;
    lv_obj_t *sys_date_label;
    lv_obj_t *sys_batt_arc;
    lv_obj_t *sys_batt_value;
    lv_obj_t *sys_batt_caption;
    lv_obj_t *sys_batt_volt;
    lv_obj_t *sys_icons[4];
    lv_obj_t *sys_icon_dots[4];
    lv_obj_t *sys_icon_captions[4];
    lv_obj_t *sys_debug_label;

    /* COMPASS */
    lv_obj_t *compass_panel;
    lv_obj_t *compass_canvas;
    lv_obj_t *compass_letters[8];
    lv_obj_t *compass_heading_label;
    lv_obj_t *compass_cardinal_label;
    lv_obj_t *compass_tilt_label;

    /* ATTITUDE */
    lv_obj_t *att_panel;
    lv_obj_t *att_heading_chip;
    lv_obj_t *att_roll_arc;
    lv_obj_t *att_roll_value[PF_BOLD_LAYERS]; /* 48px 视觉加粗层叠 */
    lv_obj_t *att_roll_caption;
    lv_obj_t *att_pitch_arc;
    lv_obj_t *att_pitch_value[PF_BOLD_LAYERS]; /* 48px 视觉加粗层叠 */
    lv_obj_t *att_pitch_caption;
    lv_obj_t *att_calib_chip; /* 校准状态/提示 */
    lv_obj_t *att_footer_label;

    /* NAV */
    lv_obj_t *nav_panel;
    lv_obj_t *nav_status_chip;
    lv_obj_t *nav_arrow_canvas;
    lv_obj_t *nav_distance_value[PF_BOLD_LAYERS]; /* 56px HUD 加粗距离 */
    lv_obj_t *nav_unit_label;
    lv_obj_t *nav_road_label;
    lv_obj_t *nav_detail_label;
    lv_obj_t *nav_remain_value;
    lv_obj_t *nav_remain_caption;
    lv_obj_t *nav_eta_value;
    lv_obj_t *nav_eta_caption;
    lv_obj_t *nav_debug_label;

    /* OBD */
    lv_obj_t *obd_panel;
    lv_obj_t *obd_status_chip;
    lv_obj_t *obd_speed_arc;
    lv_obj_t *obd_speed_value;
    lv_obj_t *obd_speed_unit;
    lv_obj_t *obd_rpm_value;
    lv_obj_t *obd_rpm_caption;
    lv_obj_t *obd_temp_value;
    lv_obj_t *obd_temp_caption;
    lv_obj_t *obd_throttle_caption;
    lv_obj_t *obd_throttle_bar;
    lv_obj_t *obd_fuel_caption;
    lv_obj_t *obd_fuel_bar;
    lv_obj_t *obd_alert_label;

    /* VOICE */
    lv_obj_t *voice_panel;
    lv_obj_t *voice_state_chip;
    lv_obj_t *voice_phase_label;
    lv_obj_t *voice_session_label;
    lv_obj_t *voice_orb;
    lv_obj_t *voice_expression_label;
    lv_obj_t *voice_ring_dots[12];
    lv_obj_t *voice_step_dots[4];
    lv_obj_t *voice_step_labels[4];
    lv_obj_t *voice_bubble_in;
    lv_obj_t *voice_bubble_in_label;
    lv_obj_t *voice_bubble_out;
    lv_obj_t *voice_bubble_out_label;
    lv_obj_t *voice_metrics_label;

    /* INCLINE (航空姿态仪, 沿用) */
    lv_obj_t *incline_panel;
    lv_obj_t *incline_canvas;
    lv_obj_t *incline_roll_label;
    lv_obj_t *incline_pitch_label;
    lv_obj_t *incline_temp_label;
    lv_obj_t *incline_ota_btn;
    lv_obj_t *incline_ota_btn_label;
} ui_widgets_t;

/* 一次性构建所有页面, 失败返回 ESP_ERR_NO_MEM */
esp_err_t ui_pages_build(lv_obj_t *screen, ui_widgets_t *w);
/* 更新顶部页面圆点指示器高亮 */
void ui_pages_set_active(ui_widgets_t *w, uint8_t page_count, uint8_t active_idx);
