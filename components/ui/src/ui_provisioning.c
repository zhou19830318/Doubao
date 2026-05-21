/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 */

/**
 * @file ui_provisioning.c
 * @brief WiFi provisioning UI with QR code display
 */

#include "ui_provisioning.h"
#include "ui.h"
#include "lvgl.h"
#include "esp_log.h"

static const char *TAG = "ui_provision";

static lv_obj_t *s_qr_screen = NULL;
static lv_obj_t *s_qr_code = NULL;
static lv_obj_t *s_hint_label = NULL;

/**
 * @brief Show provisioning QR code screen
 */
void ui_show_provisioning_qr(const char *url)
{
#if BOARD_HAS_DISPLAY
    if (!url) {
        url = "http://192.168.4.1";
    }
    
    ESP_LOGI(TAG, "Showing provisioning instructions");
    
    // Create new screen
    s_qr_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_qr_screen, lv_color_hex(0x1c1c1e), 0);
    
    // Title label
    lv_obj_t *title = lv_label_create(s_qr_screen);
    lv_label_set_text(title, "WiFi Configuration");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);
    
    // Instructions text
    s_hint_label = lv_label_create(s_qr_screen);
    lv_label_set_text(s_hint_label, 
                      "Please follow these steps:\n\n"
                      "1. Open WiFi settings\n\n"
                      "2. Connect to:\n"
                      "   AIWearable_Config\n\n"
                      "3. Browser will auto-open\n\n"
                      "4. If not, visit:\n"
                      "   http://192.168.4.1");
    lv_obj_set_style_text_color(s_hint_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_hint_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(s_hint_label, 150);
    lv_obj_align(s_hint_label, LV_ALIGN_CENTER, 0, 10);
    
    // Load screen
    lv_scr_load_anim(s_qr_screen, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
    
    ESP_LOGI(TAG, "Provisioning instructions displayed");
#else
    ESP_LOGW(TAG, "No display available for provisioning");
#endif
}

/**
 * @brief Hide provisioning QR code screen
 */
void ui_hide_provisioning_qr(void)
{
#if BOARD_HAS_DISPLAY
    if (s_qr_screen) {
        lv_obj_del(s_qr_screen);
        s_qr_screen = NULL;
        s_qr_code = NULL;
        s_hint_label = NULL;
        ESP_LOGI(TAG, "Provisioning screen hidden");
    }
#endif
}

/**
 * @brief Check if provisioning screen is showing
 */
bool ui_is_provisioning_shown(void)
{
    return (s_qr_screen != NULL);
}
