/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * Status bar module implementation - WiFi, clock, OC indicator
 */

#include "ui_status_bar.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "ui_status_bar";

/* Status bar objects */
static lv_obj_t *s_status_bar = NULL;
static lv_obj_t *s_wifi_label = NULL;
static lv_obj_t *s_clock_label = NULL;
static lv_obj_t *s_oc_dot = NULL;

/* Colors aligned with HTML simulator */
#define COLOR_WIFI_ON      lv_color_hex(0x30d158)  /* Green */
#define COLOR_WIFI_OFF     lv_color_hex(0xff453a)  /* Red */
#define COLOR_OC_ON        lv_color_hex(0x30d158)  /* Green */
#define COLOR_OC_OFF       lv_color_hex(0xff453a)  /* Red */
#define COLOR_CLOCK_TEXT   lv_color_hex(0x8e8e93)  /* Dim gray */

/* OC dot drawn as small circle (no PNG needed initially) */
static void create_oc_dot(lv_obj_t *parent)
{
    s_oc_dot = lv_obj_create(parent);
    lv_obj_set_size(s_oc_dot, 8, 8);
    lv_obj_set_style_radius(s_oc_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_oc_dot, COLOR_OC_OFF, 0);  /* Default: disconnected */
    lv_obj_set_style_border_width(s_oc_dot, 0, 0);
    lv_obj_add_flag(s_oc_dot, LV_OBJ_FLAG_CLICKABLE);
}

esp_err_t ui_status_bar_init(lv_obj_t *parent)
{
    if (!parent) {
        ESP_LOGE(TAG, "Invalid parent object");
        return ESP_ERR_INVALID_ARG;
    }

    /* Create status bar container */
    s_status_bar = lv_obj_create(parent);
    lv_obj_set_size(s_status_bar, 172, 20);
    lv_obj_align(s_status_bar, LV_ALIGN_TOP_MID, 0, 0);
    
    /* Status bar styling */
    lv_obj_set_style_bg_opa(s_status_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_status_bar, 0, 0);
    lv_obj_set_style_pad_hor(s_status_bar, 12, 0);
    lv_obj_set_style_pad_ver(s_status_bar, 2, 0);
    
    /* Use flex layout for spacing */
    lv_obj_set_flex_flow(s_status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_status_bar, 
                          LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, 
                          LV_FLEX_ALIGN_CENTER);

    /* WiFi indicator (left) */
    s_wifi_label = lv_label_create(s_status_bar);
    lv_label_set_text(s_wifi_label, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi_label, COLOR_WIFI_OFF, 0);
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_14, 0);

    /* Clock display (center) */
    s_clock_label = lv_label_create(s_status_bar);
    lv_label_set_text(s_clock_label, "12:00");
    lv_obj_set_style_text_color(s_clock_label, COLOR_CLOCK_TEXT, 0);
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_14, 0);

    /* OC connection indicator (right) */
    create_oc_dot(s_status_bar);
    
    ESP_LOGI(TAG, "Status bar initialized");
    return ESP_OK;
}

void ui_status_bar_set_wifi(bool connected, int rssi)
{
    if (!s_wifi_label) return;
    
    lv_color_t color = connected ? COLOR_WIFI_ON : COLOR_WIFI_OFF;
    lv_obj_set_style_text_color(s_wifi_label, color, 0);
    
    ESP_LOGD(TAG, "WiFi status: %s", connected ? "connected" : "disconnected");
}

void ui_status_bar_set_oc_connected(bool connected)
{
    if (!s_oc_dot) return;
    
    lv_color_t color = connected ? COLOR_OC_ON : COLOR_OC_OFF;
    lv_obj_set_style_bg_color(s_oc_dot, color, 0);
    
    ESP_LOGD(TAG, "OC status: %s", connected ? "connected" : "disconnected");
}

void ui_status_bar_set_clock(uint8_t hour, uint8_t minute)
{
    if (!s_clock_label) return;
    
    char time_str[8];  /* Increased buffer size to avoid truncation warning */
    snprintf(time_str, sizeof(time_str), "%02d:%02d", hour, minute);
    lv_label_set_text(s_clock_label, time_str);
}

lv_obj_t *ui_status_bar_get_object(void)
{
    return s_status_bar;
}
