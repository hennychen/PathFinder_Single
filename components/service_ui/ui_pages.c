#include "ui_pages.h"

#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "pf_fonts.h"
#include "ui_theme.h"

static const char *TAG = "ui_pages";

enum {
    FULL_CANVAS_BUF_SIZE = PF_CANVAS_W * PF_CANVAS_H * 2,
    ARROW_CANVAS_BUF_SIZE = PF_ARROW_W * PF_ARROW_H * 2,
    PAGE_DOT_COUNT_MAX = 8,
};

/* 共享画布缓冲: 罗盘/姿态仪共用全屏缓冲, 转向箭头用小缓冲 (页面互斥显示) */
static uint8_t *s_buf_full;
static uint8_t *s_buf_arrow;

static uint8_t *alloc_canvas_buf(size_t size)
{
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    if (buf != NULL) {
        memset(buf, 0, size);
    }
    return buf;
}

/* ===================== 顶部页面圆点指示器 ===================== */

static void build_page_dots(lv_obj_t *screen, ui_widgets_t *w)
{
    for (int i = 0; i < PAGE_DOT_COUNT_MAX; ++i) {
        w->page_dots[i] = pf_make_dot(screen, 8, PF_CLR_INACTIVE);
        lv_obj_align(w->page_dots[i], LV_ALIGN_TOP_MID, -63 + i * 18, 14);
    }
}

/* ===================== SYSTEM 表盘 ===================== */

static void build_system_page(lv_obj_t *screen, ui_widgets_t *w)
{
    w->sys_panel = pf_make_panel(screen);

    /* 状态图标行: 手机BLE / OBD / 音频 / 显示 */
    static const char *k_icons[4] = {
        LV_SYMBOL_BLUETOOTH, LV_SYMBOL_DRIVE, LV_SYMBOL_AUDIO, LV_SYMBOL_EYE_OPEN,
    };
    for (int i = 0; i < 4; ++i) {
        const int32_t cx = 98 + i * 72;
        w->sys_icons[i] = pf_make_label(w->sys_panel, &lv_font_montserrat_28, PF_CLR_FAINT);
        lv_label_set_text(w->sys_icons[i], k_icons[i]);
        lv_obj_align(w->sys_icons[i], LV_ALIGN_CENTER, cx - PF_CENTER, 46 - PF_CENTER);

        /* 图标下方小点亮状态 */
        w->sys_icon_dots[i] = pf_make_dot(w->sys_panel, 5, PF_CLR_INACTIVE);
        lv_obj_align(w->sys_icon_dots[i], LV_ALIGN_CENTER, cx - PF_CENTER, 72 - PF_CENTER);
    }

    /* 大时钟 48px */
    w->sys_time_label = pf_make_label(w->sys_panel, &lv_font_montserrat_48, PF_CLR_TEXT);
    lv_obj_align(w->sys_time_label, LV_ALIGN_CENTER, 0, 122 - PF_CENTER);

    w->sys_date_label = pf_make_label(w->sys_panel, &lv_font_montserrat_16, PF_CLR_MUTED);
    lv_obj_align(w->sys_date_label, LV_ALIGN_CENTER, 0, 162 - PF_CENTER);

    /* 电池环形仪表 190px */
    w->sys_batt_arc = pf_make_gauge(w->sys_panel, 190, 16, PF_CLR_ACCENT);
    lv_obj_align(w->sys_batt_arc, LV_ALIGN_CENTER, 0, 278 - PF_CENTER);

    w->sys_batt_value = pf_make_label(w->sys_panel, &lv_font_montserrat_36, PF_CLR_TEXT);
    lv_obj_align(w->sys_batt_value, LV_ALIGN_CENTER, 0, 266 - PF_CENTER);

    w->sys_batt_caption = pf_make_label(w->sys_panel, &lv_font_montserrat_14, PF_CLR_MUTED);
    lv_label_set_text(w->sys_batt_caption, "BATTERY");
    lv_obj_align(w->sys_batt_caption, LV_ALIGN_CENTER, 0, 300 - PF_CENTER);

    w->sys_batt_volt = pf_make_label(w->sys_panel, &lv_font_montserrat_14, PF_CLR_MUTED);
    lv_obj_align(w->sys_batt_volt, LV_ALIGN_CENTER, 0, 340 - PF_CENTER);

    w->sys_debug_label = pf_make_label(w->sys_panel, &lv_font_montserrat_14, PF_CLR_FAINT);
    lv_obj_set_width(w->sys_debug_label, 230);
    lv_label_set_long_mode(w->sys_debug_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(w->sys_debug_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(w->sys_debug_label, LV_ALIGN_CENTER, 0, 384 - PF_CENTER);
}

/* ===================== COMPASS 罗盘 ===================== */

static void build_compass_page(lv_obj_t *screen, ui_widgets_t *w)
{
    w->compass_panel = pf_make_panel(screen);

    w->compass_canvas = lv_canvas_create(w->compass_panel);
    lv_obj_set_size(w->compass_canvas, PF_CANVAS_W, PF_CANVAS_H);
    lv_obj_align(w->compass_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(w->compass_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_set_buffer(w->compass_canvas, s_buf_full, PF_CANVAS_W, PF_CANVAS_H, LV_COLOR_FORMAT_RGB565);

    /* 8 方位字母 (N/NE/E/...), 位置随航向在渲染时更新 */
    static const char *k_letters[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    for (int i = 0; i < 8; ++i) {
        if (i % 2 == 0) {
            w->compass_letters[i] = pf_make_label(w->compass_panel, &lv_font_montserrat_28, PF_CLR_TEXT);
        } else {
            w->compass_letters[i] = pf_make_label(w->compass_panel, &lv_font_montserrat_20, PF_CLR_MUTED);
        }
        lv_label_set_text(w->compass_letters[i], k_letters[i]);
    }

    /* 中心大字号航向 44px */
    w->compass_heading_label = pf_make_label(w->compass_panel, &lv_font_montserrat_44, PF_CLR_TEXT);
    lv_obj_align(w->compass_heading_label, LV_ALIGN_CENTER, 0, 168 - PF_CENTER);

    w->compass_cardinal_label = pf_make_label(w->compass_panel, &lv_font_montserrat_20, PF_CLR_TEXT_DIM);
    lv_obj_align(w->compass_cardinal_label, LV_ALIGN_CENTER, 0, 212 - PF_CENTER);

    w->compass_tilt_label = pf_make_label(w->compass_panel, &lv_font_montserrat_14, PF_CLR_MUTED);
    lv_obj_align(w->compass_tilt_label, LV_ALIGN_CENTER, 0, 240 - PF_CENTER);
}

/* ===================== ATTITUDE 姿态双弧仪表 ===================== */

static void build_attitude_page(lv_obj_t *screen, ui_widgets_t *w)
{
    w->att_panel = pf_make_panel(screen);

    w->att_heading_chip = pf_make_chip(w->att_panel, "HDG ---°", PF_CLR_ACCENT);
    lv_obj_align(w->att_heading_chip, LV_ALIGN_CENTER, 0, 46 - PF_CENTER);

    /* Roll 弧 (左) 190px */
    w->att_roll_arc = pf_make_gauge(w->att_panel, 190, 16, PF_CLR_ACCENT);
    lv_obj_align(w->att_roll_arc, LV_ALIGN_CENTER, 103 - PF_CENTER, 232 - PF_CENTER);
    /* ROLL 数值: 48px 大字 + 5 层叠描边加粗, 醒目显示 */
    pf_make_bold_label(w->att_roll_value, w->att_panel, &lv_font_montserrat_48, PF_CLR_TEXT,
                       103 - PF_CENTER, 218 - PF_CENTER);
    w->att_roll_caption = pf_make_label(w->att_panel, &lv_font_montserrat_16, PF_CLR_MUTED);
    lv_label_set_text(w->att_roll_caption, "ROLL");
    lv_obj_align(w->att_roll_caption, LV_ALIGN_CENTER, 103 - PF_CENTER, 262 - PF_CENTER);

    /* Pitch 弧 (右) 190px */
    w->att_pitch_arc = pf_make_gauge(w->att_panel, 190, 16, PF_CLR_ACCENT);
    lv_obj_align(w->att_pitch_arc, LV_ALIGN_CENTER, 309 - PF_CENTER, 232 - PF_CENTER);
    /* PITCH 数值: 48px 大字 + 5 层叠描边加粗, 醒目显示 */
    pf_make_bold_label(w->att_pitch_value, w->att_panel, &lv_font_montserrat_48, PF_CLR_TEXT,
                       309 - PF_CENTER, 218 - PF_CENTER);
    w->att_pitch_caption = pf_make_label(w->att_panel, &lv_font_montserrat_16, PF_CLR_MUTED);
    lv_label_set_text(w->att_pitch_caption, "PITCH");
    lv_obj_align(w->att_pitch_caption, LV_ALIGN_CENTER, 309 - PF_CENTER, 262 - PF_CENTER);

    /* 校准状态胶囊: 未校准提醒 / 校准中提示 */
    w->att_calib_chip = pf_make_chip(w->att_panel, LV_SYMBOL_SETTINGS " CAL", PF_CLR_AMBER);
    lv_obj_align(w->att_calib_chip, LV_ALIGN_CENTER, 0, 108 - PF_CENTER);

    w->att_footer_label = pf_make_label(w->att_panel, &lv_font_montserrat_14, PF_CLR_FAINT);
    lv_label_set_text(w->att_footer_label, "HOLD KEY 2s " LV_SYMBOL_GPS " CAL");
    lv_obj_align(w->att_footer_label, LV_ALIGN_CENTER, 0, 376 - PF_CENTER);
}

/* ===================== NAV 导航页 ===================== */

static void build_nav_page(lv_obj_t *screen, ui_widgets_t *w)
{
    w->nav_panel = pf_make_panel(screen);

    w->nav_status_chip = pf_make_chip(w->nav_panel, "NAV IDLE", PF_CLR_GRAY);
    lv_obj_align(w->nav_status_chip, LV_ALIGN_CENTER, 0, 48 - PF_CENTER);

    /* 转向箭头 canvas 240x240 (HUD 主视觉, 真实矢量图形) */
    w->nav_arrow_canvas = lv_canvas_create(w->nav_panel);
    lv_obj_set_size(w->nav_arrow_canvas, PF_ARROW_W, PF_ARROW_H);
    lv_obj_align(w->nav_arrow_canvas, LV_ALIGN_CENTER, 0, 190 - PF_CENTER);
    lv_obj_clear_flag(w->nav_arrow_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_set_buffer(w->nav_arrow_canvas, s_buf_arrow, PF_ARROW_W, PF_ARROW_H, LV_COLOR_FORMAT_RGB565);

    /* 转向说明 (箭头上方) */
    w->nav_detail_label = pf_make_label(w->nav_panel, &lv_font_montserrat_16, PF_CLR_TEXT_DIM);
    lv_obj_set_width(w->nav_detail_label, 300);
    lv_label_set_long_mode(w->nav_detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(w->nav_detail_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(w->nav_detail_label, LV_ALIGN_CENTER, 0, 92 - PF_CENTER);

    /* HUD 大号距离: 56px 粗体层叠 (转向色) */
    pf_make_bold_label(w->nav_distance_value, w->nav_panel, &pf_font_num_56, PF_CLR_TEXT,
                       0, 322 - PF_CENTER);

    w->nav_unit_label = pf_make_label(w->nav_panel, &lv_font_montserrat_20, PF_CLR_MUTED);
    lv_label_set_text(w->nav_unit_label, "m");
    lv_obj_align(w->nav_unit_label, LV_ALIGN_CENTER, 78, 322 - PF_CENTER);

    /* 路名: 34px 中文粗体 (Noto Sans SC Bold) */
    w->nav_road_label = pf_make_label(w->nav_panel, &pf_font_cn_34, PF_CLR_TEXT);
    lv_obj_set_width(w->nav_road_label, 330);
    lv_label_set_long_mode(w->nav_road_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(w->nav_road_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(w->nav_road_label, LV_ALIGN_CENTER, 0, 372 - PF_CENTER);

    /* 剩余距离 / ETA 胶囊 */
    w->nav_remain_caption = pf_make_label(w->nav_panel, &lv_font_montserrat_14, PF_CLR_MUTED);
    lv_label_set_text(w->nav_remain_caption, "REMAIN");
    lv_obj_align(w->nav_remain_caption, LV_ALIGN_CENTER, -80, 404 - PF_CENTER);
    w->nav_remain_value = pf_make_label(w->nav_panel, &lv_font_montserrat_20, PF_CLR_TEXT);
    lv_obj_align(w->nav_remain_value, LV_ALIGN_CENTER, -80, 428 - PF_CENTER);

    w->nav_eta_caption = pf_make_label(w->nav_panel, &lv_font_montserrat_14, PF_CLR_MUTED);
    lv_label_set_text(w->nav_eta_caption, "ETA");
    lv_obj_align(w->nav_eta_caption, LV_ALIGN_CENTER, 80, 404 - PF_CENTER);
    w->nav_eta_value = pf_make_label(w->nav_panel, &lv_font_montserrat_20, PF_CLR_TEXT);
    lv_obj_align(w->nav_eta_value, LV_ALIGN_CENTER, 80, 428 - PF_CENTER);

    w->nav_debug_label = pf_make_label(w->nav_panel, &lv_font_montserrat_14, PF_CLR_FAINT);
    lv_obj_set_width(w->nav_debug_label, 260);
    lv_label_set_long_mode(w->nav_debug_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(w->nav_debug_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(w->nav_debug_label, LV_ALIGN_CENTER, 0, 152 - PF_CENTER);
}

/* ===================== OBD 仪表盘 ===================== */

static void build_obd_page(lv_obj_t *screen, ui_widgets_t *w)
{
    w->obd_panel = pf_make_panel(screen);

    w->obd_status_chip = pf_make_chip(w->obd_panel, LV_SYMBOL_BLUETOOTH " ELM327 IDLE", PF_CLR_GRAY);
    lv_obj_align(w->obd_status_chip, LV_ALIGN_CENTER, 0, 42 - PF_CENTER);

    /* 速度大弧形仪表 320px (几乎满圆) */
    w->obd_speed_arc = pf_make_gauge(w->obd_panel, 320, 24, PF_CLR_ACCENT);
    lv_obj_align(w->obd_speed_arc, LV_ALIGN_CENTER, 0, 196 - PF_CENTER);

    w->obd_speed_value = pf_make_label(w->obd_panel, &lv_font_montserrat_48, PF_CLR_TEXT);
    lv_obj_align(w->obd_speed_value, LV_ALIGN_CENTER, 0, 168 - PF_CENTER);
    w->obd_speed_unit = pf_make_label(w->obd_panel, &lv_font_montserrat_20, PF_CLR_MUTED);
    lv_label_set_text(w->obd_speed_unit, "km/h");
    lv_obj_align(w->obd_speed_unit, LV_ALIGN_CENTER, 0, 218 - PF_CENTER);

    /* RPM / 水温双胶囊 36px 数值 */
    w->obd_rpm_caption = pf_make_label(w->obd_panel, &lv_font_montserrat_14, PF_CLR_MUTED);
    lv_label_set_text(w->obd_rpm_caption, "RPM");
    lv_obj_align(w->obd_rpm_caption, LV_ALIGN_CENTER, -82, 322 - PF_CENTER);
    w->obd_rpm_value = pf_make_label(w->obd_panel, &lv_font_montserrat_36, PF_CLR_TEXT);
    lv_obj_align(w->obd_rpm_value, LV_ALIGN_CENTER, -82, 354 - PF_CENTER);

    w->obd_temp_caption = pf_make_label(w->obd_panel, &lv_font_montserrat_14, PF_CLR_MUTED);
    lv_label_set_text(w->obd_temp_caption, "COOLANT");
    lv_obj_align(w->obd_temp_caption, LV_ALIGN_CENTER, 82, 322 - PF_CENTER);
    w->obd_temp_value = pf_make_label(w->obd_panel, &lv_font_montserrat_36, PF_CLR_TEXT);
    lv_obj_align(w->obd_temp_value, LV_ALIGN_CENTER, 82, 354 - PF_CENTER);

    /* 油门 / 油量条 */
    w->obd_throttle_caption = pf_make_label(w->obd_panel, &lv_font_montserrat_14, PF_CLR_MUTED);
    lv_label_set_text(w->obd_throttle_caption, "THR");
    lv_obj_align(w->obd_throttle_caption, LV_ALIGN_CENTER, -108, 396 - PF_CENTER);
    w->obd_throttle_bar = lv_bar_create(w->obd_panel);
    lv_obj_set_size(w->obd_throttle_bar, 168, 12);
    lv_obj_align(w->obd_throttle_bar, LV_ALIGN_CENTER, -6, 398 - PF_CENTER);
    lv_bar_set_range(w->obd_throttle_bar, 0, 100);
    lv_obj_set_style_bg_color(w->obd_throttle_bar, lv_color_hex(PF_CLR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(w->obd_throttle_bar, lv_color_hex(PF_CLR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(w->obd_throttle_bar, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(w->obd_throttle_bar, 6, LV_PART_INDICATOR);

    w->obd_fuel_caption = pf_make_label(w->obd_panel, &lv_font_montserrat_14, PF_CLR_MUTED);
    lv_label_set_text(w->obd_fuel_caption, "FUEL");
    lv_obj_align(w->obd_fuel_caption, LV_ALIGN_CENTER, -108, 422 - PF_CENTER);
    w->obd_fuel_bar = lv_bar_create(w->obd_panel);
    lv_obj_set_size(w->obd_fuel_bar, 168, 12);
    lv_obj_align(w->obd_fuel_bar, LV_ALIGN_CENTER, -6, 424 - PF_CENTER);
    lv_bar_set_range(w->obd_fuel_bar, 0, 100);
    lv_obj_set_style_bg_color(w->obd_fuel_bar, lv_color_hex(PF_CLR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(w->obd_fuel_bar, lv_color_hex(PF_CLR_GREEN), LV_PART_INDICATOR);
    lv_obj_set_style_radius(w->obd_fuel_bar, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(w->obd_fuel_bar, 6, LV_PART_INDICATOR);

    w->obd_alert_label = pf_make_label(w->obd_panel, &lv_font_montserrat_14, PF_CLR_TEXT_DIM);
    lv_obj_set_width(w->obd_alert_label, 250);
    lv_label_set_long_mode(w->obd_alert_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(w->obd_alert_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(w->obd_alert_label, LV_ALIGN_CENTER, 0, 262 - PF_CENTER);
}

/* ===================== VOICE 语音页 ===================== */

static void build_voice_page(lv_obj_t *screen, ui_widgets_t *w)
{
    w->voice_panel = pf_make_panel(screen);

    w->voice_state_chip = pf_make_chip(w->voice_panel, "idle", PF_CLR_GRAY);
    lv_obj_align(w->voice_state_chip, LV_ALIGN_CENTER, 0, 42 - PF_CENTER);

    w->voice_phase_label = pf_make_label(w->voice_panel, &lv_font_montserrat_16, PF_CLR_TEXT_DIM);
    lv_obj_align(w->voice_phase_label, LV_ALIGN_CENTER, 0, 74 - PF_CENTER);

    /* 语音 orb (中心) + 环绕点阵 */
    w->voice_orb = lv_obj_create(w->voice_panel);
    lv_obj_set_size(w->voice_orb, 110, 110);
    lv_obj_align(w->voice_orb, LV_ALIGN_CENTER, 0, 168 - PF_CENTER);
    lv_obj_clear_flag(w->voice_orb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(w->voice_orb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(w->voice_orb, lv_color_hex(PF_CLR_GRAY), 0);
    lv_obj_set_style_bg_opa(w->voice_orb, LV_OPA_40, 0);
    lv_obj_set_style_border_width(w->voice_orb, 4, 0);
    lv_obj_set_style_border_color(w->voice_orb, lv_color_hex(PF_CLR_GRAY), 0);

    for (int i = 0; i < 12; ++i) {
        const float a = (float)i * 30.0f;
        const float rad = a * 0.017453292519943295f;
        const int32_t x = (int32_t)(206 + 96 * sinf(rad)) - PF_CENTER;
        const int32_t y = (int32_t)(168 - 96 * cosf(rad)) - PF_CENTER;
        w->voice_ring_dots[i] = pf_make_dot(w->voice_panel, 9, PF_CLR_INACTIVE);
        lv_obj_align(w->voice_ring_dots[i], LV_ALIGN_CENTER, x, y);
    }

    w->voice_expression_label = pf_make_label(w->voice_panel, &lv_font_montserrat_20, PF_CLR_TEXT);
    lv_obj_align(w->voice_expression_label, LV_ALIGN_CENTER, 0, 168 - PF_CENTER);

    /* 步骤条: WAKE → LISTEN → THINK → SPEAK */
    static const char *k_steps[4] = {"WAKE", "LISTEN", "THINK", "SPEAK"};
    for (int i = 0; i < 4; ++i) {
        const int32_t cx = 86 + i * 80;
        w->voice_step_dots[i] = pf_make_dot(w->voice_panel, 12, PF_CLR_INACTIVE);
        lv_obj_align(w->voice_step_dots[i], LV_ALIGN_CENTER, cx - PF_CENTER, 288 - PF_CENTER);
        w->voice_step_labels[i] = pf_make_label(w->voice_panel, &lv_font_montserrat_14, PF_CLR_MUTED);
        lv_label_set_text(w->voice_step_labels[i], k_steps[i]);
        lv_obj_align(w->voice_step_labels[i], LV_ALIGN_CENTER, cx - PF_CENTER, 312 - PF_CENTER);
    }

    /* 对话气泡 (输入/输出) */
    w->voice_bubble_in = lv_obj_create(w->voice_panel);
    lv_obj_set_size(w->voice_bubble_in, 250, 52);
    lv_obj_align(w->voice_bubble_in, LV_ALIGN_CENTER, 0, 352 - PF_CENTER);
    lv_obj_clear_flag(w->voice_bubble_in, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(w->voice_bubble_in, lv_color_hex(PF_CLR_CARD), 0);
    lv_obj_set_style_border_color(w->voice_bubble_in, lv_color_hex(PF_CLR_INACTIVE), 0);
    lv_obj_set_style_border_width(w->voice_bubble_in, 1, 0);
    lv_obj_set_style_radius(w->voice_bubble_in, 16, 0);
    lv_obj_set_style_pad_all(w->voice_bubble_in, 8, 0);
    w->voice_bubble_in_label = pf_make_label(w->voice_bubble_in, &lv_font_montserrat_14, PF_CLR_MUTED);
    lv_obj_set_width(w->voice_bubble_in_label, 224);
    lv_label_set_long_mode(w->voice_bubble_in_label, LV_LABEL_LONG_DOT);
    lv_obj_center(w->voice_bubble_in_label);
    lv_label_set_text(w->voice_bubble_in_label, LV_SYMBOL_AUDIO " Say the wake word");

    w->voice_bubble_out = lv_obj_create(w->voice_panel);
    lv_obj_set_size(w->voice_bubble_out, 250, 52);
    lv_obj_align(w->voice_bubble_out, LV_ALIGN_CENTER, 0, 412 - PF_CENTER);
    lv_obj_clear_flag(w->voice_bubble_out, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(w->voice_bubble_out, lv_color_hex(PF_CLR_CARD), 0);
    lv_obj_set_style_border_color(w->voice_bubble_out, lv_color_hex(PF_CLR_INACTIVE), 0);
    lv_obj_set_style_border_width(w->voice_bubble_out, 1, 0);
    lv_obj_set_style_radius(w->voice_bubble_out, 16, 0);
    lv_obj_set_style_pad_all(w->voice_bubble_out, 8, 0);
    w->voice_bubble_out_label = pf_make_label(w->voice_bubble_out, &lv_font_montserrat_14, PF_CLR_MUTED);
    lv_obj_set_width(w->voice_bubble_out_label, 224);
    lv_label_set_long_mode(w->voice_bubble_out_label, LV_LABEL_LONG_DOT);
    lv_obj_center(w->voice_bubble_out_label);
    lv_label_set_text(w->voice_bubble_out_label, LV_SYMBOL_VOLUME_MID " Assistant idle");

    w->voice_session_label = pf_make_label(w->voice_panel, &lv_font_montserrat_14, PF_CLR_FAINT);
    lv_obj_align(w->voice_session_label, LV_ALIGN_CENTER, 0, 106 - PF_CENTER);

    w->voice_metrics_label = pf_make_label(w->voice_panel, &lv_font_montserrat_14, PF_CLR_FAINT);
    lv_obj_align(w->voice_metrics_label, LV_ALIGN_CENTER, 0, 432 - PF_CENTER);
}

/* ===================== INCLINE 航空姿态仪 (沿用) ===================== */

static void build_incline_page(lv_obj_t *screen, ui_widgets_t *w)
{
    w->incline_panel = pf_make_panel(screen);
    lv_obj_set_style_bg_color(w->incline_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(w->incline_panel, LV_OPA_COVER, 0);

    w->incline_canvas = lv_canvas_create(w->incline_panel);
    lv_obj_set_size(w->incline_canvas, PF_CANVAS_W, PF_CANVAS_H);
    lv_obj_align(w->incline_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(w->incline_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_canvas_set_buffer(w->incline_canvas, s_buf_full, PF_CANVAS_W, PF_CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(w->incline_canvas, lv_color_hex(0x3A6EA5), LV_OPA_COVER);

    w->incline_temp_label = pf_make_label(w->incline_panel, &lv_font_montserrat_16, PF_CLR_TEXT);
    lv_obj_set_width(w->incline_temp_label, 200);
    lv_obj_align(w->incline_temp_label, LV_ALIGN_CENTER, 0, 32 - PF_CENTER);
    lv_obj_set_style_text_align(w->incline_temp_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(w->incline_temp_label, "--°C");

    w->incline_roll_label = pf_make_label(w->incline_panel, &lv_font_montserrat_16, PF_CLR_TEXT);
    lv_obj_set_size(w->incline_roll_label, 120, 36);
    lv_obj_align(w->incline_roll_label, LV_ALIGN_BOTTOM_LEFT, 40, -50);
    lv_obj_set_style_text_align(w->incline_roll_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(w->incline_roll_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(w->incline_roll_label, "ROLL +0.0°");

    w->incline_pitch_label = pf_make_label(w->incline_panel, &lv_font_montserrat_16, PF_CLR_TEXT);
    lv_obj_set_size(w->incline_pitch_label, 140, 36);
    lv_obj_align(w->incline_pitch_label, LV_ALIGN_BOTTOM_RIGHT, -40, -50);
    lv_obj_set_style_text_align(w->incline_pitch_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(w->incline_pitch_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(w->incline_pitch_label, "PITCH +0.0°");

    w->incline_ota_btn = lv_btn_create(w->incline_panel);
    lv_obj_set_size(w->incline_ota_btn, 160, 36);
    lv_obj_align(w->incline_ota_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(w->incline_ota_btn, lv_color_hex(0xE89B18), 0);
    lv_obj_set_style_border_width(w->incline_ota_btn, 0, 0);
    lv_obj_set_style_radius(w->incline_ota_btn, 18, 0);
    w->incline_ota_btn_label = pf_make_label(w->incline_ota_btn, &lv_font_montserrat_16, 0x000000);
    lv_obj_center(w->incline_ota_btn_label);
    lv_obj_set_style_text_align(w->incline_ota_btn_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(w->incline_ota_btn_label, LV_SYMBOL_REFRESH " Update");
}

/* ===================== 公共构建入口 ===================== */

esp_err_t ui_pages_build(lv_obj_t *screen, ui_widgets_t *w)
{
    memset(w, 0, sizeof(*w));

    if (s_buf_full == NULL) {
        s_buf_full = alloc_canvas_buf(FULL_CANVAS_BUF_SIZE);
        if (s_buf_full == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_buf_arrow == NULL) {
        s_buf_arrow = alloc_canvas_buf(ARROW_CANVAS_BUF_SIZE);
        if (s_buf_arrow == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    build_page_dots(screen, w);
    build_system_page(screen, w);
    build_compass_page(screen, w);
    build_attitude_page(screen, w);
    build_nav_page(screen, w);
    build_obd_page(screen, w);
    build_voice_page(screen, w);
    build_incline_page(screen, w);

    return ESP_OK;
}

void ui_pages_set_active(ui_widgets_t *w, uint8_t page_count, uint8_t active_idx)
{
    for (uint8_t i = 0; i < page_count && i < PAGE_DOT_COUNT_MAX; ++i) {
        lv_obj_set_style_bg_color(w->page_dots[i],
                                  lv_color_hex(i == active_idx ? PF_CLR_ACCENT : PF_CLR_INACTIVE),
                                  0);
    }
}
