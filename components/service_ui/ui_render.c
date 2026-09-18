#include "ui_render.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "pf_fonts.h"
#include "service_voice.h"
#include "ui_theme.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

enum {
    UI_VOICE_INPUT_ACTIVITY_BYTES = 1280,
    UI_VOICE_OUTPUT_ACTIVITY_BYTES = 1920,
    COMPASS_ROSE_R = 190,
    COMPASS_LETTER_R = 150,
    VOICE_RING_DOTS = 12,
    NAV_ARROW_W = PF_ARROW_W,
    NAV_ARROW_H = PF_ARROW_H,
};

/* ===================== 跨帧缓存 ===================== */
static bool s_ai_drawn = false;
static float s_ai_last_pitch = 1000.0f;
static float s_ai_last_roll = 1000.0f;
static lv_color_t s_ai_last_safety;
static uint16_t s_compass_last_heading = 0xFFFF;
static uint8_t s_nav_last_turn = 0xFF;
static bool s_nav_arrow_drawn = false;
static uint32_t s_voice_ring_phase;
static voice_runtime_state_t s_last_voice_runtime;

/* ===================== 文案辅助 ===================== */

static const char *voice_expression_text(const workflow_state_t *state)
{
    switch ((service_voice_state_t)state->voice_runtime.state_code) {
    case SERVICE_VOICE_STATE_WAKE_DETECTED:
        return "WAKE";
    case SERVICE_VOICE_STATE_STREAMING:
        return "LISTEN";
    case SERVICE_VOICE_STATE_SPEAKING:
        return state->voice_runtime.output_silence ? "THINK" : "SPEAK";
    case SERVICE_VOICE_STATE_ERROR:
        return "ERROR";
    case SERVICE_VOICE_STATE_IDLE:
    default:
        return "READY";
    }
}

static int8_t voice_step_index(const workflow_state_t *state)
{
    switch ((service_voice_state_t)state->voice_runtime.state_code) {
    case SERVICE_VOICE_STATE_WAKE_DETECTED:
        return 0;
    case SERVICE_VOICE_STATE_STREAMING:
        return 1;
    case SERVICE_VOICE_STATE_SPEAKING:
        return state->voice_runtime.output_silence ? 2 : 3;
    case SERVICE_VOICE_STATE_ERROR:
        return -1;
    case SERVICE_VOICE_STATE_IDLE:
    default:
        return -1;
    }
}

static const char *voice_state_name(uint8_t state_code)
{
    return service_voice_state_to_string((service_voice_state_t)state_code);
}

static lv_color_t voice_state_color(uint8_t state_code)
{
    switch ((service_voice_state_t)state_code) {
    case SERVICE_VOICE_STATE_WAKE_DETECTED:
        return lv_color_hex(PF_CLR_AMBER);
    case SERVICE_VOICE_STATE_STREAMING:
        return lv_color_hex(PF_CLR_GREEN);
    case SERVICE_VOICE_STATE_SPEAKING:
        return lv_color_hex(PF_CLR_BLUE);
    case SERVICE_VOICE_STATE_ERROR:
        return lv_color_hex(PF_CLR_RED);
    case SERVICE_VOICE_STATE_IDLE:
    default:
        return lv_color_hex(PF_CLR_GRAY);
    }
}

static lv_color_t nav_turn_color(uint8_t turn_type)
{
    switch (turn_type) {
    case 0:
        return lv_color_hex(PF_CLR_BLUE);
    case 1:
    case 3:
        return lv_color_hex(PF_CLR_AMBER);
    case 2:
    case 4:
        return lv_color_hex(PF_CLR_GREEN);
    case 5:
        return lv_color_hex(PF_CLR_ORANGE);
    case 6:
        return lv_color_hex(PF_CLR_PURPLE);
    default:
        return lv_color_hex(PF_CLR_GRAY);
    }
}

/* 转向色 0xRRGGBB (供层叠标签 set_color 用) */
static uint32_t nav_turn_color_hex(uint8_t turn_type)
{
    switch (turn_type) {
    case 0:
        return PF_CLR_BLUE;
    case 1:
    case 3:
        return PF_CLR_AMBER;
    case 2:
    case 4:
        return PF_CLR_GREEN;
    case 5:
        return PF_CLR_ORANGE;
    case 6:
        return PF_CLR_PURPLE;
    default:
        return PF_CLR_GRAY;
    }
}

static lv_color_t obd_status_color(const workflow_state_t *state)
{
    if (!state->obd.connected) {
        if (strcmp(state->obd.provider_stage, "device_not_found") == 0) {
            return lv_color_hex(0xA97C50);
        }
        if (strcmp(state->obd.provider_stage, "gatt_mismatch") == 0) {
            return lv_color_hex(PF_CLR_PURPLE);
        }
        if (strcmp(state->obd.provider_stage, "connect_fail") == 0 ||
            strcmp(state->obd.provider_stage, "init_fail") == 0 ||
            strcmp(state->obd.provider_stage, "poll_fail") == 0 ||
            strcmp(state->obd.provider_stage, "response_error") == 0 ||
            strcmp(state->obd.provider_stage, "parse_fail") == 0 ||
            strcmp(state->obd.provider_stage, "write_fail") == 0 ||
            strcmp(state->obd.provider_stage, "invalid_state") == 0) {
            return lv_color_hex(PF_CLR_ORANGE);
        }
        return state->obd.provider_connect_attempt_count > 0U ? lv_color_hex(PF_CLR_AMBER)
                                                             : lv_color_hex(PF_CLR_GRAY);
    }

    return state->obd.alert_active ? lv_color_hex(PF_CLR_RED) : lv_color_hex(PF_CLR_GREEN);
}

static const char *obd_status_text(const workflow_state_t *state)
{
    if (!state->obd.connected) {
        if (strcmp(state->obd.provider_stage, "device_not_found") == 0) {
            return LV_SYMBOL_WARNING " ELM327 MISS";
        }
        if (strcmp(state->obd.provider_stage, "gatt_mismatch") == 0) {
            return LV_SYMBOL_WARNING " ELM327 GATT";
        }
        if (strcmp(state->obd.provider_stage, "connect_fail") == 0 ||
            strcmp(state->obd.provider_stage, "init_fail") == 0 ||
            strcmp(state->obd.provider_stage, "poll_fail") == 0 ||
            strcmp(state->obd.provider_stage, "response_error") == 0 ||
            strcmp(state->obd.provider_stage, "parse_fail") == 0 ||
            strcmp(state->obd.provider_stage, "write_fail") == 0 ||
            strcmp(state->obd.provider_stage, "invalid_state") == 0) {
            return LV_SYMBOL_CLOSE " ELM327 FAIL";
        }
        return state->obd.provider_connect_attempt_count > 0U ? LV_SYMBOL_REFRESH " ELM327 WAIT"
                                                             : LV_SYMBOL_BLUETOOTH " ELM327 IDLE";
    }

    return state->obd.alert_active ? LV_SYMBOL_WARNING " OBD ALERT" : LV_SYMBOL_OK " OBD LIVE";
}

static lv_color_t coolant_color(int8_t temp_c)
{
    if (temp_c >= 105) {
        return lv_color_hex(PF_CLR_RED);
    }
    if (temp_c >= 95) {
        return lv_color_hex(PF_CLR_ORANGE);
    }
    return lv_color_hex(PF_CLR_BLUE);
}

static uint32_t batt_percent_color(uint8_t percent)
{
    if (percent <= 20) {
        return PF_CLR_RED;
    }
    if (percent <= 40) {
        return PF_CLR_AMBER;
    }
    return PF_CLR_GREEN;
}

static const char *compass_cardinal(uint16_t heading_deg)
{
    static const char *k_cardinals[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return k_cardinals[(heading_deg + 22U) / 45U % 8U];
}

static const char *month_name(uint8_t month)
{
    static const char *k_months[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                       "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    return (month >= 1U && month <= 12U) ? k_months[month - 1U] : "---";
}

static void fmt_distance(uint16_t distance_m, char *buf, size_t size)
{
    if (distance_m >= 1000U) {
        snprintf(buf, size, "%lu.%lu km", (unsigned long)(distance_m / 1000U),
                 (unsigned long)((distance_m % 1000U) / 100U));
    } else {
        snprintf(buf, size, "%u m", (unsigned)distance_m);
    }
}

static void fmt_eta(uint16_t remain_s, char *buf, size_t size)
{
    const uint32_t minutes = remain_s / 60U;
    const uint32_t seconds = remain_s % 60U;
    if (minutes > 0U) {
        snprintf(buf, size, "%lum %02lus", (unsigned long)minutes, (unsigned long)seconds);
    } else {
        snprintf(buf, size, "%lus", (unsigned long)seconds);
    }
}

/* ===================== canvas: 罗盘玫瑰 ===================== */

static void compass_draw_rose(lv_obj_t *canvas, uint16_t heading_deg)
{
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    const int cx = PF_CANVAS_W / 2;
    const int cy = PF_CANVAS_H / 2;

    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_hex(PF_CLR_BG);
    bg.bg_opa = LV_OPA_COVER;
    bg.border_width = 0;
    bg.radius = 0;
    const lv_area_t full = {0, 0, PF_CANVAS_W - 1, PF_CANVAS_H - 1};
    lv_draw_rect(&layer, &bg, &full);

    /* 刻度环: 每 15° 一格 */
    lv_draw_line_dsc_t tick;
    lv_draw_line_dsc_init(&tick);
    tick.opa = LV_OPA_COVER;
    for (int deg = 0; deg < 360; deg += 15) {
        const bool is_cardinal = (deg % 90 == 0);
        const bool is_major = (deg % 45 == 0);
        const int r_outer = COMPASS_ROSE_R;
        const int r_inner = r_outer - (is_cardinal ? 22 : (is_major ? 14 : 8));
        const float a = (float)(deg - (int)heading_deg) * (float)M_PI / 180.0f;
        tick.color = lv_color_hex(is_cardinal ? PF_CLR_TEXT : (is_major ? PF_CLR_MUTED : PF_CLR_FAINT));
        tick.width = is_cardinal ? 3 : (is_major ? 2 : 1);
        tick.p1 = (lv_point_precise_t){.x = cx + r_inner * sinf(a), .y = cy - r_inner * cosf(a)};
        tick.p2 = (lv_point_precise_t){.x = cx + r_outer * sinf(a), .y = cy - r_outer * cosf(a)};
        lv_draw_line(&layer, &tick);
    }

    /* 内环装饰 */
    lv_draw_line_dsc_init(&tick);
    tick.color = lv_color_hex(PF_CLR_INACTIVE);
    tick.width = 1;
    tick.opa = LV_OPA_60;
    const int r_in = COMPASS_ROSE_R - 34;
    for (int seg = 0; seg < 72; ++seg) {
        const float a1 = (float)(seg * 5) * (float)M_PI / 180.0f;
        const float a2 = (float)(seg * 5 + 5) * (float)M_PI / 180.0f;
        tick.p1 = (lv_point_precise_t){.x = cx + r_in * cosf(a1), .y = cy + r_in * sinf(a1)};
        tick.p2 = (lv_point_precise_t){.x = cx + r_in * cosf(a2), .y = cy + r_in * sinf(a2)};
        lv_draw_line(&layer, &tick);
    }

    /* 固定顶部三角指标 (当前航向) */
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = lv_color_hex(PF_CLR_ACCENT);
    tri.opa = LV_OPA_COVER;
    tri.p[0] = (lv_point_precise_t){.x = cx - 12, .y = cy - COMPASS_ROSE_R - 16};
    tri.p[1] = (lv_point_precise_t){.x = cx + 12, .y = cy - COMPASS_ROSE_R - 16};
    tri.p[2] = (lv_point_precise_t){.x = cx, .y = cy - COMPASS_ROSE_R + 2};
    lv_draw_triangle(&layer, &tri);

    lv_canvas_finish_layer(canvas, &layer);
    lv_obj_invalidate(canvas);
}

/* ===================== canvas: 导航转向箭头 ===================== */

static void nav_fill_tri(lv_layer_t *layer, pf_pt_t p0, pf_pt_t p1, pf_pt_t p2, lv_color_t color)
{
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = color;
    tri.opa = LV_OPA_COVER;
    tri.p[0] = (lv_point_precise_t){.x = p0.x, .y = p0.y};
    tri.p[1] = (lv_point_precise_t){.x = p1.x, .y = p1.y};
    tri.p[2] = (lv_point_precise_t){.x = p2.x, .y = p2.y};
    lv_draw_triangle(layer, &tri);
}

/* 转向箭头旋转角: 0=向上, +90=向右 (屏幕顺时针) */
static int nav_turn_angle(uint8_t turn_type)
{
    switch (turn_type) {
    case 1:
        return -90;
    case 2:
        return 90;
    case 3:
        return -45;
    case 4:
        return 45;
    case 5:
        return 180;
    case 0:
    default:
        return 0;
    }
}

static void nav_draw_arrow(lv_obj_t *canvas, uint8_t turn_type)
{
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    const float cx = NAV_ARROW_W / 2.0f;
    const float cy = NAV_ARROW_H / 2.0f;
    const lv_color_t color = nav_turn_color(turn_type);

    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_hex(PF_CLR_BG);
    bg.bg_opa = LV_OPA_COVER;
    bg.border_width = 0;
    bg.radius = 0;
    const lv_area_t full = {0, 0, NAV_ARROW_W - 1, NAV_ARROW_H - 1};
    lv_draw_rect(&layer, &bg, &full);

    if (turn_type == 6) {
        /* 到达: 旗帜 (210 基准等比缩放) */
        const float s = NAV_ARROW_W / 210.0f;
        lv_draw_rect_dsc_t flag;
        lv_draw_rect_dsc_init(&flag);
        flag.bg_color = color;
        flag.bg_opa = LV_OPA_COVER;
        flag.border_width = 0;
        lv_draw_rect(&layer, &flag, &(lv_area_t){(int32_t)(cx - 42 * s), (int32_t)(cy - 63 * s), (int32_t)(cx - 34 * s), (int32_t)(cy + 63 * s)});
        lv_draw_rect(&layer, &flag, &(lv_area_t){(int32_t)(cx - 34 * s), (int32_t)(cy - 63 * s), (int32_t)(cx + 53 * s), (int32_t)(cy - 28 * s)});
        lv_draw_rect(&layer, &flag, &(lv_area_t){(int32_t)(cx - 34 * s), (int32_t)(cy - 28 * s), (int32_t)(cx + 31 * s), (int32_t)(cy + 7 * s)});
    } else {
        /* 实心转向箭头 (先画向上箭头, 再整体旋转; 坐标按画布 210 基准等比缩放) */
        const float deg = (float)nav_turn_angle(turn_type);
        const float s = NAV_ARROW_W / 210.0f;
        const pf_pt_t shaft_bl = pf_rot(cx - 22 * s, cy + 77 * s, cx, cy, deg);
        const pf_pt_t shaft_br = pf_rot(cx + 22 * s, cy + 77 * s, cx, cy, deg);
        const pf_pt_t shaft_tl = pf_rot(cx - 22 * s, cy - 14 * s, cx, cy, deg);
        const pf_pt_t shaft_tr = pf_rot(cx + 22 * s, cy - 14 * s, cx, cy, deg);
        const pf_pt_t tip = pf_rot(cx, cy - 77 * s, cx, cy, deg);
        const pf_pt_t wing_l = pf_rot(cx - 64 * s, cy - 17 * s, cx, cy, deg);
        const pf_pt_t wing_r = pf_rot(cx + 64 * s, cy - 17 * s, cx, cy, deg);

        nav_fill_tri(&layer, shaft_bl, shaft_br, shaft_tr, color);
        nav_fill_tri(&layer, shaft_bl, shaft_tr, shaft_tl, color);
        nav_fill_tri(&layer, shaft_tl, shaft_tr, tip, color);
        nav_fill_tri(&layer, shaft_tl, wing_l, tip, color);
        nav_fill_tri(&layer, shaft_tr, tip, wing_r, color);
    }

    lv_canvas_finish_layer(canvas, &layer);
    lv_obj_invalidate(canvas);
}

/* ===================== canvas: 航空姿态仪 (沿用原实现) ===================== */

static void ai_draw_horizon(lv_layer_t *layer, float pitch, float roll)
{
    const int cx = PF_CANVAS_W / 2;
    const int cy = PF_CANVAS_H / 2;

    float horizon_y = cy + pitch * 3;
    if (horizon_y < 0.0f) {
        horizon_y = 0.0f;
    }
    if (horizon_y > (float)(PF_CANVAS_H - 1)) {
        horizon_y = (float)(PF_CANVAS_H - 1);
    }
    const float rad = roll * (float)M_PI / 180.0f;
    const float cos_r = cosf(rad);
    const float sin_r = sinf(rad);

    const int half_len = 300;
    const lv_point_precise_t h1 = {.x = cx - half_len * cos_r, .y = horizon_y - half_len * sin_r};
    const lv_point_precise_t h2 = {.x = cx + half_len * cos_r, .y = horizon_y + half_len * sin_r};

    lv_draw_rect_dsc_t bg_dsc;
    lv_draw_rect_dsc_init(&bg_dsc);
    bg_dsc.bg_color = lv_color_hex(0x000000);
    bg_dsc.bg_opa = LV_OPA_COVER;
    bg_dsc.border_width = 0;
    bg_dsc.radius = 0;
    const lv_area_t full_area = {0, 0, PF_CANVAS_W - 1, PF_CANVAS_H - 1};
    lv_draw_rect(layer, &bg_dsc, &full_area);

    const int fill_far = 900;
    const lv_point_precise_t up = {.x = sin_r * fill_far, .y = -cos_r * fill_far};
    const lv_point_precise_t down = {.x = -up.x, .y = -up.y};

    lv_draw_triangle_dsc_t sky_dsc;
    lv_draw_triangle_dsc_init(&sky_dsc);
    sky_dsc.color = lv_color_hex(0x3A6EA5);
    sky_dsc.opa = LV_OPA_COVER;

    lv_draw_triangle_dsc_t gnd_dsc;
    lv_draw_triangle_dsc_init(&gnd_dsc);
    gnd_dsc.color = lv_color_hex(0x8B5A2B);
    gnd_dsc.opa = LV_OPA_COVER;

    sky_dsc.p[0] = h1;
    sky_dsc.p[1] = h2;
    sky_dsc.p[2] = (lv_point_precise_t){.x = h2.x + up.x, .y = h2.y + up.y};
    lv_draw_triangle(layer, &sky_dsc);
    sky_dsc.p[0] = h1;
    sky_dsc.p[1] = (lv_point_precise_t){.x = h1.x + up.x, .y = h1.y + up.y};
    sky_dsc.p[2] = (lv_point_precise_t){.x = h2.x + up.x, .y = h2.y + up.y};
    lv_draw_triangle(layer, &sky_dsc);

    gnd_dsc.p[0] = h1;
    gnd_dsc.p[1] = h2;
    gnd_dsc.p[2] = (lv_point_precise_t){.x = h2.x + down.x, .y = h2.y + down.y};
    lv_draw_triangle(layer, &gnd_dsc);
    gnd_dsc.p[0] = h1;
    gnd_dsc.p[1] = (lv_point_precise_t){.x = h1.x + down.x, .y = h1.y + down.y};
    gnd_dsc.p[2] = (lv_point_precise_t){.x = h2.x + down.x, .y = h2.y + down.y};
    lv_draw_triangle(layer, &gnd_dsc);

    lv_draw_line_dsc_t hz_dsc;
    lv_draw_line_dsc_init(&hz_dsc);
    hz_dsc.color = lv_color_hex(0xFFFFFF);
    hz_dsc.width = 2;
    hz_dsc.opa = LV_OPA_COVER;
    hz_dsc.p1 = h1;
    hz_dsc.p2 = h2;
    lv_draw_line(layer, &hz_dsc);

    lv_draw_line_dsc_t tick_dsc;
    lv_draw_line_dsc_init(&tick_dsc);
    tick_dsc.color = lv_color_hex(0xFFFFFF);
    tick_dsc.opa = LV_OPA_70;
    for (int deg = -30; deg <= 30; deg += 5) {
        if (deg == 0) {
            continue;
        }
        const bool is_major = (deg % 10 == 0);
        const int half_w = is_major ? 60 : 30;
        tick_dsc.width = is_major ? 2 : 1;
        const float tick_y = horizon_y - deg * 3;
        tick_dsc.p1 = (lv_point_precise_t){.x = cx - half_w * cos_r, .y = tick_y - half_w * sin_r};
        tick_dsc.p2 = (lv_point_precise_t){.x = cx + half_w * cos_r, .y = tick_y + half_w * sin_r};
        lv_draw_line(layer, &tick_dsc);
    }
}

static void ai_draw_aircraft(lv_layer_t *layer)
{
    const int cx = PF_CANVAS_W / 2;
    const int cy = PF_CANVAS_H / 2;

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = lv_color_hex(0xFFFFFF);
    dsc.width = 3;
    dsc.opa = LV_OPA_COVER;

    dsc.p1 = (lv_point_precise_t){.x = cx - 50, .y = cy + 10};
    dsc.p2 = (lv_point_precise_t){.x = cx - 15, .y = cy + 10};
    lv_draw_line(layer, &dsc);
    dsc.p1 = (lv_point_precise_t){.x = cx - 15, .y = cy + 10};
    dsc.p2 = (lv_point_precise_t){.x = cx - 15, .y = cy};
    lv_draw_line(layer, &dsc);
    dsc.p1 = (lv_point_precise_t){.x = cx + 15, .y = cy};
    dsc.p2 = (lv_point_precise_t){.x = cx + 15, .y = cy + 10};
    lv_draw_line(layer, &dsc);
    dsc.p1 = (lv_point_precise_t){.x = cx + 15, .y = cy + 10};
    dsc.p2 = (lv_point_precise_t){.x = cx + 50, .y = cy + 10};
    lv_draw_line(layer, &dsc);

    lv_draw_rect_dsc_t dot_dsc;
    lv_draw_rect_dsc_init(&dot_dsc);
    dot_dsc.bg_color = lv_color_hex(0xFFFFFF);
    dot_dsc.bg_opa = LV_OPA_COVER;
    dot_dsc.border_width = 0;
    dot_dsc.radius = LV_RADIUS_CIRCLE;
    lv_draw_rect(layer, &dot_dsc, &(lv_area_t){cx - 4, cy - 4, cx + 4, cy + 4});
}

static void ai_draw_roll_pointer(lv_layer_t *layer, lv_color_t color)
{
    const int cx = PF_CANVAS_W / 2;

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = 3;
    dsc.opa = LV_OPA_COVER;

    const lv_point_precise_t tri[] = {
        {.x = cx - 10, .y = 12}, {.x = cx + 10, .y = 12}, {.x = cx, .y = 32}
    };
    dsc.p1 = tri[0];
    dsc.p2 = tri[1];
    lv_draw_line(layer, &dsc);
    dsc.p1 = tri[1];
    dsc.p2 = tri[2];
    lv_draw_line(layer, &dsc);
    dsc.p1 = tri[2];
    dsc.p2 = tri[0];
    lv_draw_line(layer, &dsc);
}

static void ai_draw_roll_scale(lv_layer_t *layer)
{
    const int cx = PF_CANVAS_W / 2;
    const int cy = PF_CANVAS_H / 2;
    const int r = 190;

    lv_draw_line_dsc_t tick_dsc;
    lv_draw_line_dsc_init(&tick_dsc);
    tick_dsc.color = lv_color_hex(0xA0AEC0);
    tick_dsc.opa = LV_OPA_COVER;

    for (int deg = -60; deg <= 60; deg += 10) {
        const bool is_major = (deg % 30 == 0);
        tick_dsc.width = is_major ? 2 : 1;
        const int inner_r = r - (is_major ? 14 : 8);
        const float a = (deg + 180) * (float)M_PI / 180.0f;
        tick_dsc.p1 = (lv_point_precise_t){.x = cx + inner_r * sinf(a), .y = cy - inner_r * cosf(a)};
        tick_dsc.p2 = (lv_point_precise_t){.x = cx + r * sinf(a), .y = cy - r * cosf(a)};
        lv_draw_line(layer, &tick_dsc);
    }
}

static lv_color_t ai_safety_color(float pitch, float roll)
{
    float max_angle = fabsf(pitch);
    if (fabsf(roll) > max_angle) {
        max_angle = fabsf(roll);
    }
    if (max_angle > 60.0f) {
        return lv_color_hex(0xFF5252);
    }
    if (max_angle > 45.0f) {
        return lv_color_hex(0xFFC107);
    }
    return lv_color_hex(0xFFCC00);
}

/* ===================== 页面渲染: SYSTEM ===================== */

static void render_system(const workflow_state_t *state, ui_widgets_t *w)
{
    char buf[96];

    /* 大时钟 */
    snprintf(buf, sizeof(buf), "%02u:%02u", state->rtc_hour, state->rtc_minute);
    lv_label_set_text(w->sys_time_label, buf);
    snprintf(buf, sizeof(buf), "%s %u  " LV_SYMBOL_OK, month_name(state->rtc_month), state->rtc_day);
    lv_label_set_text(w->sys_date_label, buf);

    /* 电池弧 */
    lv_arc_set_value(w->sys_batt_arc, state->battery_percent);
    lv_obj_set_style_arc_color(w->sys_batt_arc, lv_color_hex(batt_percent_color(state->battery_percent)),
                               LV_PART_INDICATOR);
    snprintf(buf, sizeof(buf), "%u%%", state->battery_percent);
    lv_label_set_text(w->sys_batt_value, buf);
    snprintf(buf, sizeof(buf), "%umV  RTC %s", (unsigned)state->battery_voltage_mv,
             state->rtc_valid ? "OK" : "--");
    lv_label_set_text(w->sys_batt_volt, buf);

    /* 状态图标 (语义色: 亮=活动, 灰=空闲, 红=告警) */
    lv_obj_set_style_text_color(w->sys_icons[0],
                                lv_color_hex(state->ble_phone_connected ? PF_CLR_BLUE : PF_CLR_FAINT), 0);
    lv_obj_set_style_text_color(w->sys_icons[1],
                                lv_color_hex(state->obd.connected ? PF_CLR_GREEN
                                      : (state->obd.alert_active ? PF_CLR_RED : PF_CLR_FAINT)), 0);
    lv_obj_set_style_text_color(w->sys_icons[2],
                                lv_color_hex((state->audio.owner_text[0] != '\0') ? PF_CLR_AMBER : PF_CLR_FAINT), 0);
    lv_obj_set_style_text_color(w->sys_icons[3],
                                lv_color_hex(state->power.backlight_dimmed ? PF_CLR_FAINT : PF_CLR_MUTED), 0);

    /* 调试信息一行化 (UI/RTC/音频/策略) */
    snprintf(buf, sizeof(buf),
             "UI %s | %s | IDLE %lums | SLEEP %s",
             state->ui_ready ? "READY" : "WAIT",
             state->audio.policy_text[0] != '\0' ? state->audio.policy_text : "audio idle",
             (unsigned long)state->power.idle_duration_ms,
             state->power.light_sleep_ready ? "OK" : "WAIT");
    lv_label_set_text(w->sys_debug_label, buf);
}

/* ===================== 页面渲染: COMPASS ===================== */

static void render_compass(const workflow_state_t *state, ui_widgets_t *w)
{
    const uint16_t heading = state->heading_deg;

    /* 增量重绘: 航向变化 ≥2° 才重画 canvas */
    if (s_compass_last_heading == 0xFFFF || heading != s_compass_last_heading) {
        const uint16_t clamped = (heading < 2U || heading > 358U || (heading > s_compass_last_heading + 1U) ||
                                  (heading + 1U < s_compass_last_heading))
                                     ? heading
                                     : s_compass_last_heading;
        if (clamped == s_compass_last_heading && s_compass_last_heading != 0xFFFF) {
            /* 微小抖动 (<2°) 不重绘 */
        } else {
            compass_draw_rose(w->compass_canvas, heading);
            s_compass_last_heading = heading;
        }
    }

    /* 8 方位字母位置环绕中心排布 */
    for (int i = 0; i < 8; ++i) {
        const float a = (float)(i * 45 - (int)heading) * (float)M_PI / 180.0f;
        const float r = COMPASS_LETTER_R;
        const int32_t x = (int32_t)(PF_CENTER + r * sinf(a)) - PF_CENTER;
        const int32_t y = (int32_t)(PF_CENTER - r * cosf(a)) - PF_CENTER;
        lv_obj_align(w->compass_letters[i], LV_ALIGN_CENTER, x, y);
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%u°", heading);
    lv_label_set_text(w->compass_heading_label, buf);
    lv_label_set_text(w->compass_cardinal_label, compass_cardinal(heading));
    snprintf(buf, sizeof(buf), "R %+.0f  P %+.0f", (double)state->roll_deg, (double)state->pitch_deg);
    lv_label_set_text(w->compass_tilt_label, buf);
}

/* ===================== 页面渲染: ATTITUDE ===================== */

static void render_attitude(const workflow_state_t *state, ui_widgets_t *w)
{
    char buf[48];

    snprintf(buf, sizeof(buf), "HDG %u°", state->heading_deg);
    lv_label_set_text(w->att_heading_chip, buf);

    /* Roll/Pitch 弧: ±60° → 0-100 */
    const int32_t roll_val = (int32_t)((state->roll_deg + 60.0f) * 100.0f / 120.0f);
    const int32_t pitch_val = (int32_t)((state->pitch_deg + 60.0f) * 100.0f / 120.0f);
    lv_arc_set_value(w->att_roll_arc, roll_val);
    lv_arc_set_value(w->att_pitch_arc, pitch_val);

    snprintf(buf, sizeof(buf), "%+.1f°", (double)state->roll_deg);
    pf_bold_label_set_text(w->att_roll_value, buf);
    snprintf(buf, sizeof(buf), "%+.1f°", (double)state->pitch_deg);
    pf_bold_label_set_text(w->att_pitch_value, buf);

    /* 超限警示色 (>45° 橙, >60° 红) */
    const float max_angle = fmaxf(fabsf(state->roll_deg), fabsf(state->pitch_deg));
    const uint32_t warn = max_angle > 60.0f ? PF_CLR_RED : (max_angle > 45.0f ? PF_CLR_ORANGE : 0);
    if (warn != 0) {
        lv_obj_set_style_arc_color(w->att_roll_arc, lv_color_hex(warn), LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(w->att_pitch_arc, lv_color_hex(warn), LV_PART_INDICATOR);
        pf_bold_label_set_color(w->att_roll_value, warn);
        pf_bold_label_set_color(w->att_pitch_value, warn);
    } else {
        lv_obj_set_style_arc_color(w->att_roll_arc, lv_color_hex(PF_CLR_BLUE), LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(w->att_pitch_arc, lv_color_hex(PF_CLR_GREEN), LV_PART_INDICATOR);
        pf_bold_label_set_color(w->att_roll_value, PF_CLR_TEXT);
        pf_bold_label_set_color(w->att_pitch_value, PF_CLR_TEXT);
    }

    /* 校准状态胶囊: 校准中呼吸提示 / 已校准隐藏 / 未校准提醒 */
    if (state->attitude_calibrating) {
        lv_obj_clear_flag(w->att_calib_chip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(w->att_calib_chip, lv_color_hex(PF_CLR_ACCENT), 0);
        lv_label_set_text(w->att_calib_chip, LV_SYMBOL_REFRESH " CALIB...");
    } else if (!state->attitude_calibrated) {
        lv_obj_clear_flag(w->att_calib_chip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(w->att_calib_chip, lv_color_hex(PF_CLR_AMBER), 0);
        lv_label_set_text(w->att_calib_chip, LV_SYMBOL_SETTINGS " UNCAL");
    } else {
        lv_obj_add_flag(w->att_calib_chip, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ===================== 页面渲染: NAV ===================== */

static const char *nav_turn_detail_text(uint8_t turn_type)
{
    switch (turn_type) {
    case 0:
        return "Keep straight on the current road";
    case 1:
        return "Prepare to turn left";
    case 2:
        return "Prepare to turn right";
    case 3:
        return "Bear left at the next fork";
    case 4:
        return "Bear right at the next fork";
    case 5:
        return "Make a U-turn when possible";
    case 6:
        return "Approaching destination";
    default:
        return "Awaiting a valid navigation frame";
    }
}

static void render_nav(const workflow_state_t *state, ui_widgets_t *w)
{
    char buf[96];
    char step_buf[24];
    char total_buf[24];
    char eta_buf[24];

    const lv_color_t accent = state->navigation.active ? nav_turn_color(state->navigation.turn_type)
                                                       : lv_color_hex(PF_CLR_GRAY);
    fmt_distance(state->navigation.step_distance_m, step_buf, sizeof(step_buf));
    fmt_distance(state->navigation.total_distance_m, total_buf, sizeof(total_buf));
    fmt_eta(state->navigation.remain_time_s, eta_buf, sizeof(eta_buf));

    lv_obj_set_style_bg_color(w->nav_status_chip, accent, 0);
    lv_label_set_text(w->nav_status_chip, state->navigation.active
                          ? (state->ble_phone_connected ? LV_SYMBOL_BLUETOOTH " PHONE LIVE" : "NAV DEMO")
                          : "NAV IDLE");

    /* 转向箭头: 转向类型变化时重绘 */
    const uint8_t turn = state->navigation.active ? state->navigation.turn_type : 0xFF;
    if (turn != s_nav_last_turn || !s_nav_arrow_drawn) {
        nav_draw_arrow(w->nav_arrow_canvas, turn);
        s_nav_last_turn = turn;
        s_nav_arrow_drawn = true;
    }

    /* 大号距离 (数字 + 单位分开渲染, 数字 32px) */
    if (state->navigation.active) {
        if (state->navigation.step_distance_m >= 1000U) {
            snprintf(buf, sizeof(buf), "%lu.%lu", (unsigned long)(state->navigation.step_distance_m / 1000U),
                     (unsigned long)((state->navigation.step_distance_m % 1000U) / 100U));
            lv_label_set_text(w->nav_unit_label, "km");
        } else {
            snprintf(buf, sizeof(buf), "%u", (unsigned)state->navigation.step_distance_m);
            lv_label_set_text(w->nav_unit_label, "m");
        }
    } else {
        snprintf(buf, sizeof(buf), "--");
        lv_label_set_text(w->nav_unit_label, "m");
    }
    pf_bold_label_set_text(w->nav_distance_value, buf);
    pf_bold_label_set_color(w->nav_distance_value,
                            state->navigation.active
                                ? nav_turn_color_hex(state->navigation.turn_type)
                                : PF_CLR_GRAY);

    lv_label_set_text(w->nav_road_label, state->navigation.road_name[0] != '\0'
                          ? state->navigation.road_name
                          : (state->navigation.active ? "Unnamed road" : "Waiting for route"));

    lv_label_set_text(w->nav_detail_label, state->navigation.active
                          ? nav_turn_detail_text(state->navigation.turn_type)
                          : "Waiting for BLE navigation frames");

    lv_label_set_text(w->nav_remain_value, state->navigation.active ? total_buf : "--");
    lv_label_set_text(w->nav_eta_value, state->navigation.active ? eta_buf : "--");

    /* 调试行: 包计数 */
    snprintf(buf, sizeof(buf), "%s %lu | err %lu",
             state->navigation.active ? "PKT" : "pkt",
             (unsigned long)state->navigation.packet_count,
             (unsigned long)state->navigation.invalid_packet_count);
    lv_label_set_text(w->nav_debug_label, buf);
}

/* ===================== 页面渲染: OBD ===================== */

static void render_obd(const workflow_state_t *state, ui_widgets_t *w)
{
    char buf[64];
    char detail_buf[96];

    const lv_color_t accent = obd_status_color(state);

    lv_obj_set_style_bg_color(w->obd_status_chip, accent, 0);
    lv_label_set_text(w->obd_status_chip, obd_status_text(state));

    /* 速度大弧 (0-240 km/h) */
    const uint16_t speed = state->obd.connected ? state->obd.speed_kmh : 0;
    lv_arc_set_value(w->obd_speed_arc, LV_MIN(100, (int32_t)speed * 100 / 240));
    snprintf(buf, sizeof(buf), "%u", state->obd.connected ? speed : 0U);
    lv_label_set_text(w->obd_speed_value, buf);

    /* 速度色: 高速橙红 */
    if (speed >= 120) {
        lv_obj_set_style_arc_color(w->obd_speed_arc, lv_color_hex(PF_CLR_ORANGE), LV_PART_INDICATOR);
        lv_obj_set_style_text_color(w->obd_speed_value, lv_color_hex(PF_CLR_ORANGE), 0);
    } else {
        lv_obj_set_style_arc_color(w->obd_speed_arc, lv_color_hex(PF_CLR_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_text_color(w->obd_speed_value, lv_color_hex(PF_CLR_TEXT), 0);
    }

    /* RPM / 水温 */
    snprintf(buf, sizeof(buf), "%u", state->obd.connected ? state->obd.rpm : 0U);
    lv_label_set_text(w->obd_rpm_value, buf);
    snprintf(buf, sizeof(buf), "%d°", state->obd.connected ? state->obd.coolant_temp_c : 0);
    lv_label_set_text(w->obd_temp_value, buf);
    lv_obj_set_style_text_color(w->obd_temp_value,
                                coolant_color(state->obd.connected ? state->obd.coolant_temp_c : 0), 0);

    /* 油门 / 油量 */
    lv_bar_set_value(w->obd_throttle_bar, state->obd.connected ? state->obd.throttle_percent : 0, LV_ANIM_ON);
    lv_bar_set_value(w->obd_fuel_bar, state->obd.connected ? state->obd.fuel_percent : 0, LV_ANIM_ON);

    /* 告警/状态摘要 */
    const char *detail = NULL;
    if (!state->obd.connected && state->obd.provider_detail[0] != '\0') {
        detail = state->obd.provider_detail;
    } else if (state->obd.alert_text[0] != '\0') {
        detail = state->obd.alert_text;
    }
    if (detail != NULL) {
        snprintf(detail_buf, sizeof(detail_buf), "%s", detail);
        lv_label_set_text(w->obd_alert_label, detail_buf);
    } else {
        lv_label_set_text(w->obd_alert_label,
                          state->obd.connected ? LV_SYMBOL_OK " Session live" : "Waiting for ELM327 session");
    }
}

/* ===================== 页面渲染: VOICE ===================== */

static void render_voice(const workflow_state_t *state, ui_widgets_t *w)
{
    const uint32_t input_delta = (state->voice_runtime.session_count == s_last_voice_runtime.session_count)
                                     ? (state->voice_runtime.input_bytes - s_last_voice_runtime.input_bytes)
                                     : state->voice_runtime.input_bytes;
    const uint32_t output_delta = (state->voice_runtime.session_count == s_last_voice_runtime.session_count)
                                      ? (state->voice_runtime.output_bytes - s_last_voice_runtime.output_bytes)
                                      : state->voice_runtime.output_bytes;
    const int32_t input_level = LV_MIN(100, (int32_t)((input_delta * 100U) / UI_VOICE_INPUT_ACTIVITY_BYTES));
    const int32_t output_level = LV_MIN(100, (int32_t)((output_delta * 100U) / UI_VOICE_OUTPUT_ACTIVITY_BYTES));
    const int32_t activity = LV_MAX(input_level, output_level);
    const lv_color_t accent = voice_state_color(state->voice_runtime.state_code);
    const int8_t step_idx = voice_step_index(state);
    char buf[128];

    /* orb 呼吸: 活动时放大 */
    const int32_t orb_size = state->voice_active ? 114 + (activity / 4) : 110;
    lv_obj_set_size(w->voice_orb, orb_size, orb_size);
    lv_obj_align(w->voice_orb, LV_ALIGN_CENTER, 0, 168 - PF_CENTER);
    lv_obj_set_style_bg_color(w->voice_orb, accent, 0);
    lv_obj_set_style_bg_opa(w->voice_orb, state->voice_active ? LV_OPA_60 : LV_OPA_30, 0);
    lv_obj_set_style_border_color(w->voice_orb, accent, 0);
    lv_obj_set_style_border_width(w->voice_orb, state->voice_active ? 4 : 2, 0);

    /* 环绕点阵旋转点亮 (活动时) */
    ++s_voice_ring_phase;
    for (int i = 0; i < VOICE_RING_DOTS; ++i) {
        bool lit = false;
        if (state->voice_active) {
            const int lit_count = 2 + (activity * 10) / 100;
            const int offset = (int)(s_voice_ring_phase / 4) % VOICE_RING_DOTS;
            for (int k = 0; k < lit_count; ++k) {
                if ((offset + k) % VOICE_RING_DOTS == i) {
                    lit = true;
                    break;
                }
            }
        }
        lv_obj_set_style_bg_color(w->voice_ring_dots[i],
                                  lv_color_hex(lit ? (i % 3 == 0 ? PF_CLR_ACCENT : PF_CLR_TEXT) : PF_CLR_INACTIVE),
                                  0);
    }

    /* 步骤条 */
    for (int i = 0; i < 4; ++i) {
        lv_obj_set_style_bg_color(w->voice_step_dots[i],
                                  lv_color_hex(i == step_idx ? PF_CLR_ACCENT : PF_CLR_INACTIVE), 0);
        lv_obj_set_style_text_color(w->voice_step_labels[i],
                                    lv_color_hex(i == step_idx ? PF_CLR_TEXT : PF_CLR_FAINT), 0);
    }

    lv_obj_set_style_bg_color(w->voice_state_chip, accent, 0);
    lv_label_set_text(w->voice_state_chip, voice_state_name(state->voice_runtime.state_code));
    lv_label_set_text(w->voice_expression_label, voice_expression_text(state));

    /* 气泡 */
    const bool input_active = (state->voice_runtime.state_code == SERVICE_VOICE_STATE_WAKE_DETECTED) ||
                              (state->voice_runtime.state_code == SERVICE_VOICE_STATE_STREAMING);
    const bool output_active = (state->voice_runtime.state_code == SERVICE_VOICE_STATE_SPEAKING);
    lv_obj_set_style_border_color(w->voice_bubble_in,
                                  lv_color_hex(input_active ? PF_CLR_GREEN : PF_CLR_INACTIVE), 0);
    lv_obj_set_style_border_color(w->voice_bubble_out,
                                  lv_color_hex(output_active ? PF_CLR_BLUE : PF_CLR_INACTIVE), 0);
    lv_label_set_text(w->voice_bubble_in_label,
                      state->voice_runtime.input_text[0] != '\0'
                          ? state->voice_runtime.input_text
                          : (input_active ? LV_SYMBOL_AUDIO " Listening..." : LV_SYMBOL_AUDIO " Say the wake word"));
    lv_label_set_text(w->voice_bubble_out_label,
                      state->voice_runtime.output_text[0] != '\0'
                          ? state->voice_runtime.output_text
                          : (output_active ? LV_SYMBOL_VOLUME_MAX " Speaking..." : LV_SYMBOL_VOLUME_MID " Assistant idle"));

    snprintf(buf, sizeof(buf), "S%lu %lums", (unsigned long)state->voice_runtime.session_count,
             (unsigned long)state->voice_runtime.session_duration_ms);
    lv_label_set_text(w->voice_session_label, buf);
    snprintf(buf, sizeof(buf), "RX %lu B/s  TX %lu B/s",
             (unsigned long)(state->voice_runtime.session_duration_ms > 0U
                                 ? (state->voice_runtime.input_bytes * 1000ULL /
                                    state->voice_runtime.session_duration_ms)
                                 : 0ULL),
             (unsigned long)(state->voice_runtime.session_duration_ms > 0U
                                 ? (state->voice_runtime.output_bytes * 1000ULL /
                                    state->voice_runtime.session_duration_ms)
                                 : 0ULL));
    lv_label_set_text(w->voice_metrics_label, buf);

    s_last_voice_runtime = state->voice_runtime;
}

/* ===================== 页面渲染: INCLINE (航空姿态仪) ===================== */

static void render_incline(const workflow_state_t *state, ui_widgets_t *w)
{
    const lv_color_t safety = ai_safety_color(state->pitch_deg, state->roll_deg);

    float pitch = state->pitch_deg;
    if (pitch > 90.0f) {
        pitch = 90.0f;
    }
    if (pitch < -90.0f) {
        pitch = -90.0f;
    }
    float roll = state->roll_deg;
    while (roll > 180.0f) {
        roll -= 360.0f;
    }
    while (roll < -180.0f) {
        roll += 360.0f;
    }

    /* 增量跳帧 */
    const bool need_redraw = !s_ai_drawn || fabsf(pitch - s_ai_last_pitch) >= 0.1f ||
                             fabsf(roll - s_ai_last_roll) >= 0.1f || !lv_color_eq(safety, s_ai_last_safety);
    if (need_redraw) {
        lv_layer_t layer;
        lv_canvas_init_layer(w->incline_canvas, &layer);
        ai_draw_horizon(&layer, pitch, roll);
        ai_draw_roll_scale(&layer);
        ai_draw_aircraft(&layer);
        ai_draw_roll_pointer(&layer, safety);
        lv_canvas_finish_layer(w->incline_canvas, &layer);

        s_ai_drawn = true;
        s_ai_last_pitch = pitch;
        s_ai_last_roll = roll;
        s_ai_last_safety = safety;
        lv_obj_invalidate(w->incline_canvas);
    }

    lv_label_set_text_fmt(w->incline_roll_label, "ROLL %+.1f°", (double)state->roll_deg);
    lv_label_set_text_fmt(w->incline_pitch_label, "PITCH %+.1f°", (double)state->pitch_deg);
    lv_label_set_text_fmt(w->incline_temp_label, "%.0f°C", (double)state->imu_temperature_c);
    lv_obj_set_style_text_color(w->incline_roll_label, safety, 0);
    lv_obj_set_style_text_color(w->incline_pitch_label, safety, 0);
}

/* ===================== 页面调度 ===================== */

void ui_render_page(app_page_id_t page, const workflow_state_t *state, ui_widgets_t *w)
{
    /* 页面可见性切换: 先取各面板当前可见性, 无缝切换避免"全隐藏中间态"
     * (旧逻辑先隐藏全部再显示目标页, 切换瞬间全屏 invalidate + canvas
     * 尚未重绘, 表现为黑屏闪烁)。 */
    const bool sys_shown = !lv_obj_has_flag(w->sys_panel, LV_OBJ_FLAG_HIDDEN);
    const bool compass_shown = !lv_obj_has_flag(w->compass_panel, LV_OBJ_FLAG_HIDDEN);
    const bool att_shown = !lv_obj_has_flag(w->att_panel, LV_OBJ_FLAG_HIDDEN);
    const bool nav_shown = !lv_obj_has_flag(w->nav_panel, LV_OBJ_FLAG_HIDDEN);
    const bool obd_shown = !lv_obj_has_flag(w->obd_panel, LV_OBJ_FLAG_HIDDEN);
    const bool voice_shown = !lv_obj_has_flag(w->voice_panel, LV_OBJ_FLAG_HIDDEN);
    const bool incline_shown = !lv_obj_has_flag(w->incline_panel, LV_OBJ_FLAG_HIDDEN);

    /* 同页刷新: 直接渲染, 不动可见性 */
    if ((page == PAGE_SYSTEM && sys_shown) || (page == PAGE_COMPASS && compass_shown)
        || (page == PAGE_ATTITUDE && att_shown) || (page == PAGE_NAV && nav_shown)
        || (page == PAGE_OBD && obd_shown) || (page == PAGE_VOICE && voice_shown)
        || (page == PAGE_INCLINE && incline_shown)) {
        /* fallthrough 渲染, 见下方 switch */
    } else {
        /* 跨页切换: 先渲染新页内容(canvas/label 全部就位并显示), 再隐藏旧页,
         * 同一 LVGL 帧内完成, 显示器只在帧末收到一次完整脏区刷新 */
        switch (page) {
        case PAGE_SYSTEM:
            lv_obj_clear_flag(w->sys_panel, LV_OBJ_FLAG_HIDDEN);
            render_system(state, w);
            break;
        case PAGE_COMPASS:
            lv_obj_clear_flag(w->compass_panel, LV_OBJ_FLAG_HIDDEN);
            render_compass(state, w);
            break;
        case PAGE_ATTITUDE:
            lv_obj_clear_flag(w->att_panel, LV_OBJ_FLAG_HIDDEN);
            render_attitude(state, w);
            break;
        case PAGE_NAV:
            lv_obj_clear_flag(w->nav_panel, LV_OBJ_FLAG_HIDDEN);
            render_nav(state, w);
            break;
        case PAGE_OBD:
            lv_obj_clear_flag(w->obd_panel, LV_OBJ_FLAG_HIDDEN);
            render_obd(state, w);
            break;
        case PAGE_VOICE:
            lv_obj_clear_flag(w->voice_panel, LV_OBJ_FLAG_HIDDEN);
            render_voice(state, w);
            break;
        case PAGE_INCLINE:
            lv_obj_clear_flag(w->incline_panel, LV_OBJ_FLAG_HIDDEN);
            render_incline(state, w);
            break;
        default:
            lv_obj_clear_flag(w->sys_panel, LV_OBJ_FLAG_HIDDEN);
            render_system(state, w);
            break;
        }

        /* 旧页在渲染完成后隐藏: 除目标页外全部隐藏。
         * lv_obj_add_flag 幂等(已隐藏的重复隐藏无副作用), 无需再判断
         * 之前的可见性快照。 */
        if (page != PAGE_SYSTEM) {
            lv_obj_add_flag(w->sys_panel, LV_OBJ_FLAG_HIDDEN);
        }
        if (page != PAGE_COMPASS) {
            lv_obj_add_flag(w->compass_panel, LV_OBJ_FLAG_HIDDEN);
        }
        if (page != PAGE_ATTITUDE) {
            lv_obj_add_flag(w->att_panel, LV_OBJ_FLAG_HIDDEN);
        }
        if (page != PAGE_NAV) {
            lv_obj_add_flag(w->nav_panel, LV_OBJ_FLAG_HIDDEN);
        }
        if (page != PAGE_OBD) {
            lv_obj_add_flag(w->obd_panel, LV_OBJ_FLAG_HIDDEN);
        }
        if (page != PAGE_VOICE) {
            lv_obj_add_flag(w->voice_panel, LV_OBJ_FLAG_HIDDEN);
        }
        if (page != PAGE_INCLINE) {
            lv_obj_add_flag(w->incline_panel, LV_OBJ_FLAG_HIDDEN);
        }

        ui_pages_set_active(w, PAGE_COUNT, (uint8_t)page);
        return;
    }

    /* 同页刷新路径 */
    switch (page) {
    case PAGE_SYSTEM:
        render_system(state, w);
        break;
    case PAGE_COMPASS:
        render_compass(state, w);
        break;
    case PAGE_ATTITUDE:
        render_attitude(state, w);
        break;
    case PAGE_NAV:
        render_nav(state, w);
        break;
    case PAGE_OBD:
        render_obd(state, w);
        break;
    case PAGE_VOICE:
        render_voice(state, w);
        break;
    case PAGE_INCLINE:
        render_incline(state, w);
        break;
    default:
        render_system(state, w);
        break;
    }

    ui_pages_set_active(w, PAGE_COUNT, (uint8_t)page);
}
