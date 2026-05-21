/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * State chip module implementation - Bottom state indicator
 */

#include "ui_state_chip.h"
#include "esp_log.h"

extern const lv_font_t SourceHanSansCN_Medium_16;

static const char *TAG = "ui_state_chip";

/* State chip objects */
static lv_obj_t *s_state_chip = NULL;
static lv_obj_t *s_state_label = NULL;
static lv_obj_t *s_state_chinese = NULL;

/* Default colors */
#define COLOR_CHIP_BG        lv_color_hex(0x161B22)  /* Dark background */
#define COLOR_CHIP_TEXT      lv_color_hex(0xe0e0e0)  /* Light text */

esp_err_t ui_state_chip_init(lv_obj_t *parent)
{
    if (!parent) {
        ESP_LOGE(TAG, "Invalid parent object");
        return ESP_ERR_INVALID_ARG;
    }

    /* Create state chip container */
    s_state_chip = lv_obj_create(parent);
    lv_obj_set_size(s_state_chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);  /* Auto size */
    lv_obj_align(s_state_chip, LV_ALIGN_BOTTOM_MID, 0, -30);  /* Above button bar */
    
    /* Chip styling - aligned with HTML simulator */
    lv_obj_set_style_bg_color(s_state_chip, COLOR_CHIP_BG, 0);
    lv_obj_set_style_bg_opa(s_state_chip, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_state_chip, 20, 0);  /* Capsule shape */
    lv_obj_set_style_border_width(s_state_chip, 0, 0);
    lv_obj_set_style_pad_hor(s_state_chip, 36, 0);  /* padding: 3px 36px */
    lv_obj_set_style_pad_ver(s_state_chip, 3, 0);
    lv_obj_set_style_pad_all(s_state_chip, 0, 0);
    
    /* Use flex layout for label */
    lv_obj_set_flex_flow(s_state_chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_state_chip, 
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, 
                          LV_FLEX_ALIGN_CENTER);

    /* State name label - 12px font (smallest available) */
    s_state_label = lv_label_create(s_state_chip);
    lv_label_set_text(s_state_label, "BOOT");
    lv_obj_set_style_text_color(s_state_label, COLOR_CHIP_TEXT, 0);
    lv_obj_set_style_text_font(s_state_label, &lv_font_montserrat_12, 0);  /* 12px */
    lv_obj_set_style_text_align(s_state_label, LV_TEXT_ALIGN_CENTER, 0);

    /* Chinese state label - below chip, no background */
    s_state_chinese = lv_label_create(parent);
    lv_label_set_text(s_state_chinese, "系统启动中");
    lv_obj_set_style_text_font(s_state_chinese, &SourceHanSansCN_Medium_16, 0);
    lv_obj_set_style_text_color(s_state_chinese, lv_color_hex(0x8e8e93), 0);
    lv_obj_set_style_bg_opa(s_state_chinese, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_state_chinese, 0, 0);
    lv_obj_set_style_text_align(s_state_chinese, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_state_chinese, LV_ALIGN_BOTTOM_MID, 0, -8);  /* Below chip, no background */
    
    ESP_LOGI(TAG, "State chip initialized");
    return ESP_OK;
}

void ui_state_chip_update(const char *state_label, const char *chinese_text, lv_color_t color)
{
    if (!s_state_label || !s_state_chinese) return;
    
    /* Update state name */
    if (state_label) {
        lv_label_set_text(s_state_label, state_label);
    }
    
    /* Update Chinese text */
    if (chinese_text) {
        lv_label_set_text(s_state_chinese, chinese_text);
    }
    
    /* Update accent color (background tint) - chip only, Chinese label has no background */
    if (color.full != 0) {
        /* Blend color with dark background for subtle effect */
        lv_obj_set_style_bg_color(s_state_chip, color, 0);
    }
    
    ESP_LOGD(TAG, "State updated: %s / %s", 
             state_label ? state_label : "(null)",
             chinese_text ? chinese_text : "(null)");
}

lv_obj_t *ui_state_chip_get_object(void)
{
    return s_state_chip;
}
