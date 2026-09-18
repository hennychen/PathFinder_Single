#pragma once

#include <math.h>

#include "lvgl.h"

/* ===================== 设计令牌 (纯黑底白字主题) ===================== */
#define PF_CLR_BG        0x000000 /* 纯黑 */
#define PF_CLR_CARD      0x0D0D0D /* 卡片底 */
#define PF_CLR_STROKE    0x262626 /* 描边 */
#define PF_CLR_TRACK     0x1F1F1F /* 弧形轨道 */
#define PF_CLR_TEXT      0xFFFFFF
#define PF_CLR_TEXT_DIM  0xE6E6E6
#define PF_CLR_MUTED     0xB3B3B3
#define PF_CLR_FAINT     0x666666
#define PF_CLR_INACTIVE  0x333333
#define PF_CLR_GREEN     0x30D158 /* 活跃/正常 */
#define PF_CLR_BLUE      0xFFFFFF /* 主强调色: 白 */
#define PF_CLR_AMBER     0xFFD60A /* 提醒 */
#define PF_CLR_ORANGE    0xFF9F0A /* 警告 */
#define PF_CLR_RED       0xFF453A /* 告警 */
#define PF_CLR_PURPLE    0xB56BFF
#define PF_CLR_GRAY      0x8E8E93
#define PF_CLR_ACCENT    0xFFFFFF /* 全局强调 = 白, 与黑底一体 */

/* 屏幕: 412x412 圆形 */
#define PF_CENTER 206

/* 共享画布尺寸 (RGB565) */
#define PF_CANVAS_W 412
#define PF_CANVAS_H 412

/* 导航转向箭头画布尺寸 */
#define PF_ARROW_W 240
#define PF_ARROW_H 240

typedef struct {
    float x;
    float y;
} pf_pt_t;

static inline pf_pt_t pf_pt(float x, float y)
{
    pf_pt_t p = {x, y};
    return p;
}

/* 2D 点绕 (cx,cy) 旋转 (屏幕坐标, y 向下, 角度顺时针为正) */
static inline pf_pt_t pf_rot(float x, float y, float cx, float cy, float deg)
{
    const float rad = deg * 0.017453292519943295f;
    const float s = sinf(rad);
    const float c = cosf(rad);
    const float dx = x - cx;
    const float dy = y - cy;
    pf_pt_t p = {cx + dx * c - dy * s, cy + dx * s + dy * c};
    return p;
}

/* 极坐标 → 屏幕 (0° 指向正上, 顺时针) */
static inline pf_pt_t pf_polar(float cx, float cy, float r, float deg)
{
    const float rad = deg * 0.017453292519943295f;
    pf_pt_t p = {cx + r * sinf(rad), cy - r * cosf(rad)};
    return p;
}

static inline lv_obj_t *pf_make_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    /* LVGL 基础对象默认尺寸为 LV_DPI_DEF(130px), 必须显式铺满屏幕,
     * 否则以 CENTER 对齐的子控件会基于左上角小方块计算偏移 */
    lv_obj_set_pos(panel, 0, 0);
    lv_obj_set_size(panel, PF_CANVAS_W, PF_CANVAS_H);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    return panel;
}

static inline lv_obj_t *pf_make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

/* ===================== 视觉加粗标签 ===================== */
/* 内置 Montserrat 无粗体变体, 用主层 + 4 向 1px 偏移层叠加实现描边加粗 */
enum {
    PF_BOLD_LAYERS = 5,
};

static inline void pf_make_bold_label(lv_obj_t **layers,
                                      lv_obj_t *parent,
                                      const lv_font_t *font,
                                      uint32_t color,
                                      int32_t x_ofs,
                                      int32_t y_ofs)
{
    static const int8_t k_off[PF_BOLD_LAYERS - 1][2] = {
        {-1, 0}, {1, 0}, {0, -1}, {0, 1},
    };
    for (int i = 0; i < PF_BOLD_LAYERS; ++i) {
        layers[i] = pf_make_label(parent, font, color);
        const int32_t ox = (i == 0) ? x_ofs : x_ofs + k_off[i - 1][0];
        const int32_t oy = (i == 0) ? y_ofs : y_ofs + k_off[i - 1][1];
        lv_obj_align(layers[i], LV_ALIGN_CENTER, ox, oy);
    }
}

static inline void pf_bold_label_set_text(lv_obj_t **layers, const char *text)
{
    for (int i = 0; i < PF_BOLD_LAYERS; ++i) {
        lv_label_set_text(layers[i], text);
    }
}

static inline void pf_bold_label_set_color(lv_obj_t **layers, uint32_t color)
{
    for (int i = 0; i < PF_BOLD_LAYERS; ++i) {
        lv_obj_set_style_text_color(layers[i], lv_color_hex(color), 0);
    }
}

static inline lv_obj_t *pf_make_chip(lv_obj_t *parent, const char *text, uint32_t color)
{
    lv_obj_t *chip = pf_make_label(parent, &lv_font_montserrat_16, PF_CLR_TEXT);
    lv_label_set_text(chip, text);
    lv_obj_set_style_bg_color(chip, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chip, 13, 0);
    lv_obj_set_style_pad_hor(chip, 14, 0);
    lv_obj_set_style_pad_ver(chip, 5, 0);
    return chip;
}

/* 手表风格弧形仪表: 270° 扫掠, 135° 起始 */
static inline lv_obj_t *pf_make_gauge(lv_obj_t *parent, int32_t size, int32_t width, uint32_t accent)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(PF_CLR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(arc, 0, 0);
    return arc;
}

static inline lv_obj_t *pf_make_dot(lv_obj_t *parent, int32_t size, uint32_t color)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_size(dot, size, size);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    return dot;
}
