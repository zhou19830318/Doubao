/* SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 */

/**
 * @file ui_provisioning.c
 * @brief WiFi provisioning screen with QR code
 *
 * When the device enters AP mode (no WiFi configured or STA connect failed),
 * this screen displays:
 *   1. A QR code encoding WiFi credentials (WIFI:T:nopass;S:Doubao_Config;;)
 *      — phone cameras / WiFi apps scan this to auto-connect to the AP.
 *   2. Text instructions as fallback for phones without QR scanning.
 *
 * After connecting to the AP, the captive portal redirects to the web config
 * page at http://192.168.4.1 where the user can set WiFi credentials + API keys.
 */

#include "ui_provisioning.h"
#include "ui.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_provision";

static lv_obj_t *s_qr_screen = NULL;

/* WiFi QR code format per https://github.com/zxing/zxing/wiki/Wifi-Qrcode */
static void build_wifi_qr_string(char *buf, size_t cap,
                                  const char *ssid, const char *password)
{
    /* WIFI:T:<auth>;S:<ssid>;P:<password>;;  — open network: T:nopass, P omitted */
    const char *auth = "nopass";  /* AP is WIFI_AUTH_OPEN */
    if (password && password[0]) {
        auth = "WPA";  /* future-proof if AP gets a password */
        snprintf(buf, cap, "WIFI:T:WPA;S:%s;P:%s;;", ssid, password);
    } else {
        snprintf(buf, cap, "WIFI:T:nopass;S:%s;;", ssid);
    }
}

/**
 * @brief Show provisioning QR code screen
 *
 * @param url  Custom URL (NULL → default http://192.168.4.1)
 *             The QR code always encodes WiFi credentials for auto-connect.
 */
void ui_show_provisioning_qr(const char *url)
{
#if BOARD_HAS_DISPLAY
    (void)url; /* URL is not used for the QR — we encode WiFi creds instead */

    ESP_LOGI(TAG, "Showing WiFi provisioning screen");

    /* ── Screen ── */
    s_qr_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_qr_screen, lv_color_hex(0x1c1c1e), 0);
    lv_obj_set_style_bg_opa(s_qr_screen, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_qr_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_qr_screen, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_qr_screen, 8, 0);
    lv_obj_set_style_pad_all(s_qr_screen, 12, 0);

    /* ── Title ── */
    lv_obj_t *title = lv_label_create(s_qr_screen);
    lv_label_set_text(title, "WiFi Configuration");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);

    /* ── QR code ── */
    lv_obj_t *qr = lv_qrcode_create(s_qr_screen);
    lv_qrcode_set_size(qr, 180);                     /* 180×180 px on 410px-wide display */
    lv_qrcode_set_dark_color(qr, lv_color_white());   /* dark modules = white on dark bg */
    lv_qrcode_set_light_color(qr, lv_color_hex(0x1c1c1e));  /* light modules = bg color */
    lv_qrcode_set_quiet_zone(qr, true);

    char wifi_str[128];
    build_wifi_qr_string(wifi_str, sizeof(wifi_str), "Doubao_Config", NULL);
    lv_result_t res = lv_qrcode_update(qr, wifi_str, strlen(wifi_str));
    if (res != LV_RESULT_OK) {
        ESP_LOGE(TAG, "QR code generation failed (data too long?)");
    }

    /* ── Hint text ── */
    lv_obj_t *hint = lv_label_create(s_qr_screen);
    lv_label_set_text(hint,
        "1. Scan QR with phone camera\n"
        "2. Tap to connect to WiFi\n"
        "3. Open browser → 192.168.4.1\n"
        "\n"
        "Or connect to:\n"
        "  SSID: Doubao_Config");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xbbbbbb), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(hint, LV_PCT(90));

    /* ── Load ── */
    lv_screen_load_anim(s_qr_screen, LV_SCREEN_LOAD_ANIM_FADE_ON, 300, 0, false);
    ESP_LOGI(TAG, "Provisioning screen displayed with WiFi QR code");
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
