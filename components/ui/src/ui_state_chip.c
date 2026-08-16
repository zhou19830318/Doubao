/*
 * SPDX-FileCopyrightText: 2024-2026 AIWatch Contributors
 * SPDX-License-Identifier: MIT
 *
 * Dynamic Island + Status Bar module
 * iPhone 17 style — top row: WiFi/OC status, below: state pill
 *
 * Layout (y, portrait 410×502):
 *   y=32..48  Status bar: WiFi icon (left) · OC dot (right) — 与灵动岛同行(中心 y=40)
 *   y=18..62  Dynamic Island pill: [STATE] (centered, 250×44)
 *   y=72..92  Chinese subtitle (16px)
 */

#include "ui_state_chip.h"
#include "board.h"
#include "esp_log.h"
#include <stdio.h>

extern const lv_font_t SourceHanSansCN_Medium_16;

static const char *TAG = "ui_dynamic_island";

/* ── LVGL objects ──────────────────────────────────────────────────────── */
static lv_obj_t *s_status_bar = NULL;
static lv_obj_t *s_wifi_label = NULL;
static lv_obj_t *s_oc_dot = NULL;
static lv_obj_t *s_island = NULL;
static lv_obj_t *s_state_label = NULL;
static lv_obj_t *s_state_chinese = NULL;

/* ── Colors ────────────────────────────────────────────────────────────── */
#define COLOR_ISLAND_BG     lv_color_hex(0x1c1c1e)
#define COLOR_ISLAND_BORDER lv_color_hex(0x38383a)
#define COLOR_TEXT_WHITE    lv_color_hex(0xffffff)
#define COLOR_TEXT_DIM      lv_color_hex(0x8e8e93)
#define COLOR_WIFI_ON       lv_color_hex(0x30d158)  /* Green */
#define COLOR_WIFI_OFF      lv_color_hex(0xff453a)  /* Red */
#define COLOR_OC_ON         lv_color_hex(0x30d158)  /* Green */
#define COLOR_OC_OFF        lv_color_hex(0x48484a)  /* Gray */

/* ══════════════════════════════════════════════════════════════════════════
 * Initialization
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t ui_state_chip_init(lv_obj_t *parent)
{
    if (!parent) {
        ESP_LOGE(TAG, "Invalid parent object");
        return ESP_ERR_INVALID_ARG;
    }

    /* ── 1. Status bar row (与灵动岛同一行, 垂直中心 y=40) ─────────────── */
    s_status_bar = lv_obj_create(parent);
    lv_obj_set_size(s_status_bar, BOARD_LCD_H_RES, 16);
    /* y=32..48, 中心 y=40 = 灵动岛胶囊 (y=18..62) 的中心, 图标不再被顶部圆角裁剪 */
    lv_obj_align(s_status_bar, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_status_bar, 0, 0);
    /* WiFi / OC 图标向内平移 30px（20 → 50, 避开 AMOLED 屏幕圆角裁剪）。
     * 按板宽等比缩放: AMOLED-2.06 (410px) = 50px, 音频板 (172px) ≈ 21px。
     * ⚠ 顺序不能反: pad_all 会同时覆盖 left/right/top/bottom,
     *   必须放在 pad_hor 之前, 否则 50px 内边距会被清成 0,
     *   图标回到屏幕最边缘, 被圆角裁掉完全看不到（曾导致图标消失）。 */
    lv_obj_set_style_pad_all(s_status_bar, 0, 0);
    lv_obj_set_style_pad_hor(s_status_bar, (BOARD_LCD_H_RES * 50 + 205) / 410, 0);
    lv_obj_remove_flag(s_status_bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Flex: WiFi left, OC dot right */
    lv_obj_set_flex_flow(s_status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_status_bar,
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* WiFi icon (left) */
    s_wifi_label = lv_label_create(s_status_bar);
    lv_label_set_text(s_wifi_label, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi_label, COLOR_WIFI_OFF, 0);
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_14, 0);

    /* OC dot (right) */
    s_oc_dot = lv_obj_create(s_status_bar);
    lv_obj_set_size(s_oc_dot, 6, 6);
    lv_obj_set_style_radius(s_oc_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_oc_dot, COLOR_OC_OFF, 0);
    lv_obj_set_style_border_width(s_oc_dot, 0, 0);
    lv_obj_set_style_bg_opa(s_oc_dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_oc_dot, LV_OBJ_FLAG_SCROLLABLE);

    /* ── 2. Dynamic Island pill (y=18..62, 410x502 screen) ─────────────── */
    s_island = lv_obj_create(parent);
    /* 410x502 screen: fixed 250x44 pill, full-round capsule */
    lv_obj_set_size(s_island, 250, 44);
    lv_obj_align(s_island, LV_ALIGN_TOP_MID, 0, 18);

    /* Pill styling */
    lv_obj_set_style_bg_color(s_island, COLOR_ISLAND_BG, 0);
    lv_obj_set_style_bg_opa(s_island, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_island, 22, 0);  /* 全圆角胶囊 */
    lv_obj_set_style_border_width(s_island, 1, 0);
    lv_obj_set_style_border_color(s_island, COLOR_ISLAND_BORDER, 0);
    lv_obj_set_style_shadow_width(s_island, 8, 0);
    lv_obj_set_style_shadow_color(s_island, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(s_island, LV_OPA_40, 0);
    /* pad_all 必须在前, 否则会覆盖后面的 pad_hor（与状态栏同样的问题） */
    lv_obj_set_style_pad_all(s_island, 0, 0);
    lv_obj_set_style_pad_hor(s_island, 16, 0);
    lv_obj_remove_flag(s_island, LV_OBJ_FLAG_SCROLLABLE);

    /* Center the state label inside the pill */
    lv_obj_set_flex_flow(s_island, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_island,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    s_state_label = lv_label_create(s_island);
    lv_label_set_text(s_state_label, "BOOT");
    lv_obj_set_style_text_color(s_state_label, COLOR_TEXT_WHITE, 0);
    lv_obj_set_style_text_font(s_state_label, &lv_font_montserrat_14, 0);

    /* ── 3. Chinese subtitle (below island, y=72) ─────────────────────── */
    /* Island bottom is y=62; subtitle at y=72 keeps a 10px gap (410x502 grid) */
    s_state_chinese = lv_label_create(parent);
    lv_label_set_text(s_state_chinese, "系统启动");
    lv_obj_set_style_text_font(s_state_chinese, &SourceHanSansCN_Medium_16, 0);
    lv_obj_set_style_text_color(s_state_chinese, COLOR_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_state_chinese, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_state_chinese, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_style_bg_opa(s_state_chinese, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_state_chinese, 0, 0);

    ESP_LOGI(TAG, "Status bar + Dynamic Island initialized");
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════════ */

void ui_state_chip_update(const char *state_label, const char *chinese_text, lv_color_t color)
{
    if (s_state_label && state_label) {
        lv_label_set_text(s_state_label, state_label);
    }
    if (s_state_chinese && chinese_text) {
        lv_label_set_text(s_state_chinese, chinese_text);
    }
    /* Accent border glow */
    if (s_island && lv_color_to_u32(color) != 0) {
        lv_obj_set_style_border_color(s_island, color, 0);
        lv_obj_set_style_shadow_color(s_island, color, 0);
        lv_obj_set_style_shadow_opa(s_island, LV_OPA_30, 0);
    }

    ESP_LOGD(TAG, "Island state: %s / %s",
             state_label ? state_label : "(null)",
             chinese_text ? chinese_text : "(null)");
}

void ui_state_chip_set_wifi(bool connected)
{
    if (!s_wifi_label) return;
    lv_obj_set_style_text_color(s_wifi_label,
                                connected ? COLOR_WIFI_ON : COLOR_WIFI_OFF, 0);
}

void ui_state_chip_set_clock(uint8_t hour, uint8_t minute)
{
    /* Clock removed from Dynamic Island by user request — no-op */
    (void)hour;
    (void)minute;
}

void ui_state_chip_set_oc(bool connected)
{
    if (!s_oc_dot) return;
    lv_obj_set_style_bg_color(s_oc_dot,
                              connected ? COLOR_OC_ON : COLOR_OC_OFF, 0);
    lv_obj_set_style_shadow_width(s_oc_dot, connected ? 6 : 0, 0);
    lv_obj_set_style_shadow_color(s_oc_dot, COLOR_OC_ON, 0);
    lv_obj_set_style_shadow_opa(s_oc_dot, connected ? LV_OPA_50 : 0, 0);
}

lv_obj_t *ui_state_chip_get_object(void)
{
    return s_island;
}
