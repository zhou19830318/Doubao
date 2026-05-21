/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * Tasks screen — shows OpenClaw cron jobs in a scrollable list
 *
 * Navigation:
 *   - Wheel click → back to main screen
 *   - Touch empty area → back to main screen
 *   - Wheel spin → scroll task list
 *   - Touch task → show detail / stop running task
 */

#include "ui_tasks.h"
#include "ui.h"
#include "board.h"

#if !BOARD_HAS_DISPLAY
/* Stubs for screenless boards */
#include "esp_log.h"
esp_err_t ui_tasks_init(void) { return ESP_OK; }
void ui_tasks_set_event_group(void *eg) { (void)eg; }
void ui_tasks_show(void) {}
void ui_tasks_hide(void) {}
bool ui_tasks_is_visible(void) { return false; }
void ui_tasks_scroll(int d) { (void)d; }
void ui_tasks_refresh(void) {}
lv_obj_t *ui_tasks_get_screen(void) { return NULL; }
#else /* BOARD_HAS_DISPLAY */

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "openclaw_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ui_tasks";

/* Colors (shared with main ui.c) */
#define C_BG            lv_color_hex(0x0D1117)
#define C_BAR_BG        lv_color_hex(0x161B22)
#define C_TEXT          lv_color_hex(0xE6EDF3)
#define C_TEXT_DIM      lv_color_hex(0x7D8590)
#define C_BORDER        lv_color_hex(0x30363D)
#define C_GREEN         lv_color_hex(0x3FB950)
#define C_RED           lv_color_hex(0xF85149)
#define C_ORANGE        lv_color_hex(0xD29922)
#define C_BLUE          lv_color_hex(0x58A6FF)
#define C_PURPLE        lv_color_hex(0xBC8CFF)
#define C_TEAL          lv_color_hex(0x39D353)

/* Screen objects */
static lv_obj_t *s_tasks_scr = NULL;
static lv_obj_t *s_title_label = NULL;
static lv_obj_t *s_task_list = NULL;      /* scrollable container */
static lv_obj_t *s_back_hint = NULL;
static lv_obj_t *s_detail_panel = NULL;   /* detail overlay */
static lv_obj_t *s_detail_name = NULL;
static lv_obj_t *s_detail_info = NULL;

static bool s_visible = false;
static bool s_detail_visible = false;
static EventGroupHandle_t s_events = NULL;

/* Event bits — must match app_state.h */
#define UI_TASKS_BACK_BIT  BIT2   /* reuse KNOB_PRESSED for "back" */
#define UI_TOUCH_BIT       BIT7

/* Forward declarations */
static void back_cb(lv_event_t *e);
static void task_item_cb(lv_event_t *e);

void ui_tasks_set_event_group(void *event_group)
{
    s_events = (EventGroupHandle_t)event_group;
}

esp_err_t ui_tasks_init(void)
{
    lv_disp_t *disp = board_get_lvgl_disp();
    if (!disp) return ESP_FAIL;
    if (!lvgl_port_lock(1000)) return ESP_FAIL;

    s_tasks_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_tasks_scr, C_BG, 0);
    lv_obj_set_style_bg_opa(s_tasks_scr, LV_OPA_COVER, 0);

    /* Touch empty area to go back - only on the screen background, not on task list */
    lv_obj_add_flag(s_tasks_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_tasks_scr, back_cb, LV_EVENT_CLICKED, NULL);

    /* Title */
    s_title_label = lv_label_create(s_tasks_scr);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_title_label, C_TEXT, 0);
    lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_title_label, 160);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 14);
    lv_label_set_text(s_title_label, "Tasks");

    /* Scrollable task list container */
    s_task_list = lv_obj_create(s_tasks_scr);
    lv_obj_set_size(s_task_list, 160, 250);
    lv_obj_align(s_task_list, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_set_style_bg_opa(s_task_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_task_list, 0, 0);
    lv_obj_set_style_pad_all(s_task_list, 4, 0);
    lv_obj_set_flex_flow(s_task_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_task_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_task_list, 6, 0);
    /* Allow scrolling */
    lv_obj_add_flag(s_task_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_task_list, LV_DIR_VER);
    /* Prevent click events from propagating to parent screen's back_cb */
    lv_obj_add_flag(s_task_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_task_list, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    /* CRITICAL: Stop event propagation so clicking tasks doesn't trigger back */
    lv_obj_add_flag(s_task_list, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* Back hint at bottom */
    s_back_hint = lv_label_create(s_tasks_scr);
    lv_obj_set_style_text_font(s_back_hint, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_back_hint, C_TEXT_DIM, 0);
    lv_obj_set_style_text_align(s_back_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_back_hint, 160);
    lv_obj_align(s_back_hint, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_label_set_text(s_back_hint, "Tap to go back");

    /* Detail overlay (hidden by default) */
    s_detail_panel = lv_obj_create(s_tasks_scr);
    lv_obj_set_size(s_detail_panel, 158, 286);
    lv_obj_align(s_detail_panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_detail_panel, C_BAR_BG, 0);
    lv_obj_set_style_bg_opa(s_detail_panel, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_detail_panel, 20, 0);
    lv_obj_set_style_border_width(s_detail_panel, 2, 0);
    lv_obj_set_style_border_color(s_detail_panel, C_BORDER, 0);
    lv_obj_set_style_pad_all(s_detail_panel, 16, 0);
    lv_obj_clear_flag(s_detail_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detail_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_detail_panel, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_detail_panel, LV_OBJ_FLAG_HIDDEN);

    s_detail_name = lv_label_create(s_detail_panel);
    lv_obj_set_style_text_font(s_detail_name, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_detail_name, C_TEXT, 0);
    lv_obj_set_width(s_detail_name, 140);
    lv_obj_set_style_text_align(s_detail_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_detail_name, LV_ALIGN_TOP_MID, 0, 10);

    s_detail_info = lv_label_create(s_detail_panel);
    lv_obj_set_style_text_font(s_detail_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_detail_info, C_TEXT_DIM, 0);
    lv_obj_set_width(s_detail_info, 140);
    lv_obj_set_style_text_align(s_detail_info, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_detail_info, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_detail_info, LV_ALIGN_TOP_MID, 0, 42);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "Tasks screen initialized");
    return ESP_OK;
}

/* ── Navigation ──────────────────────────────────────────────────────── */

static void back_cb(lv_event_t *e)
{
    (void)e;
    if (s_detail_visible) {
        /* Close detail panel */
        s_detail_visible = false;
        if (lvgl_port_lock(100)) {
            lv_obj_add_flag(s_detail_panel, LV_OBJ_FLAG_HIDDEN);
            lvgl_port_unlock();
        }
        return;
    }
    /* Go back to main screen */
    ui_tasks_hide();
}

void ui_tasks_show(void)
{
    if (s_visible || !s_tasks_scr) return;
    if (!lvgl_port_lock(200)) return;
    lv_disp_t *disp = board_get_lvgl_disp();
    if (disp) {
        lv_scr_load(s_tasks_scr);
    }
    s_visible = true;
    s_detail_visible = false;
    lvgl_port_unlock();

    /* Refresh task list */
    ui_tasks_refresh();
    ESP_LOGI(TAG, "Tasks screen shown");
}

void ui_tasks_hide(void)
{
    if (!s_visible) return;
    s_visible = false;
    s_detail_visible = false;
    if (!lvgl_port_lock(200)) return;
    lv_obj_t *main_scr = ui_get_main_screen();
    if (main_scr) {
        lv_scr_load(main_scr);
    }
    lvgl_port_unlock();
    ESP_LOGI(TAG, "Tasks screen hidden");
}

bool ui_tasks_is_visible(void)
{
    return s_visible;
}

void ui_tasks_scroll(int direction)
{
    if (!s_visible || !s_task_list || s_detail_visible) return;
    if (!lvgl_port_lock(100)) return;
    lv_coord_t scroll = direction > 0 ? -40 : 40;
    lv_obj_scroll_by(s_task_list, 0, scroll, LV_ANIM_ON);
    lvgl_port_unlock();
}

/* ── Task item click ─────────────────────────────────────────────────── */

static void task_item_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const openclaw_info_t *info = openclaw_get_info();
    if (!info || idx < 0 || idx >= info->task_count) return;

    const openclaw_task_t *t = &info->tasks[idx];
    ESP_LOGI(TAG, "Task tapped: %s (running=%d enabled=%d)", t->name, t->running, t->enabled);

    /* If task is running or enabled, disable it. If disabled, enable it. */
    if (t->running || t->enabled) {
        ESP_LOGI(TAG, "Disabling task: %s", t->id);
        openclaw_cron_toggle(t->id, false);
    } else {
        ESP_LOGI(TAG, "Enabling task: %s", t->id);
        openclaw_cron_toggle(t->id, true);
    }

    /* Refresh after a brief delay */
    vTaskDelay(pdMS_TO_TICKS(500));
    openclaw_request_tasks();

    if (!lvgl_port_lock(200)) return;

    /* Show detail panel */
    lv_label_set_text(s_detail_name, t->name);

    /* Re-read info after toggle */
    info = openclaw_get_info();
    if (!info || idx >= info->task_count) { lvgl_port_unlock(); return; }
    t = &info->tasks[idx];

    char detail[256];
    int pos = 0;
    pos += snprintf(detail + pos, sizeof(detail) - pos, "Status: %s\n",
                    t->running ? "RUNNING" : (t->enabled ? "Active" : "Disabled"));
    pos += snprintf(detail + pos, sizeof(detail) - pos, "Schedule: %s %s\n",
                    t->schedule_kind, t->schedule_expr);
    if (t->last_status[0]) {
        pos += snprintf(detail + pos, sizeof(detail) - pos, "Last: %s", t->last_status);
        if (t->last_duration_ms > 0) {
            pos += snprintf(detail + pos, sizeof(detail) - pos, " (%dms)", t->last_duration_ms);
        }
        pos += snprintf(detail + pos, sizeof(detail) - pos, "\n");
    }
    if (t->last_error[0]) {
        pos += snprintf(detail + pos, sizeof(detail) - pos, "Error: %.60s\n", t->last_error);
    }
    if (t->consecutive_errors > 0) {
        pos += snprintf(detail + pos, sizeof(detail) - pos, "Consecutive errors: %d\n", t->consecutive_errors);
    }
    pos += snprintf(detail + pos, sizeof(detail) - pos, "\nTap to %s / tap elsewhere to close",
                    t->enabled ? "disable" : "enable");

    lv_label_set_text(s_detail_info, detail);
    lv_obj_set_style_text_color(s_detail_name,
                                 t->running ? C_ORANGE : (t->enabled ? C_GREEN : C_TEXT_DIM), 0);

    lv_obj_clear_flag(s_detail_panel, LV_OBJ_FLAG_HIDDEN);
    s_detail_visible = true;

    lvgl_port_unlock();
}

/* ── Refresh task list ────────────────────────────────────────────────── */

void ui_tasks_refresh(void)
{
    if (!s_visible || !s_task_list) return;
    const openclaw_info_t *info = openclaw_get_info();
    if (!info) return;

    if (!lvgl_port_lock(300)) return;

    /* Clear existing list items */
    lv_obj_clean(s_task_list);

    if (info->task_count == 0) {
        lv_obj_t *empty = lv_label_create(s_task_list);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(empty, C_TEXT_DIM, 0);
        lv_label_set_text(empty, "No scheduled tasks");
        lv_obj_set_width(empty, 150);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        /* Update title with count */
        char title[48];
        snprintf(title, sizeof(title), "Tasks (%d)", info->task_count);
        lv_label_set_text(s_title_label, title);

        for (int i = 0; i < info->task_count; i++) {
            const openclaw_task_t *t = &info->tasks[i];

            /* Task row container */
            lv_obj_t *row = lv_obj_create(s_task_list);
            lv_obj_set_size(row, 150, 48);
            lv_obj_set_style_bg_color(row, C_BAR_BG, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_80, 0);
            lv_obj_set_style_radius(row, 10, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, C_BORDER, 0);
            lv_obj_set_style_border_opa(row, LV_OPA_40, 0);
            lv_obj_set_style_pad_hor(row, 10, 0);
            lv_obj_set_style_pad_ver(row, 4, 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_opa(row, LV_OPA_60, LV_STATE_PRESSED);
            lv_obj_add_event_cb(row, task_item_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);

            /* Status dot */
            lv_obj_t *dot = lv_obj_create(row);
            lv_obj_set_size(dot, 10, 10);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_color_t dot_color;
            if (t->running) dot_color = C_ORANGE;
            else if (t->enabled) dot_color = C_GREEN;
            else dot_color = C_TEXT_DIM;
            lv_obj_set_style_bg_color(dot, dot_color, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);

            /* Task name */
            lv_obj_t *name_lbl = lv_label_create(row);
            lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(name_lbl, C_TEXT, 0);
            lv_label_set_text(name_lbl, t->name);
            lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_width(name_lbl, 90);
            lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 14, -7);

            /* Schedule / status line */
            lv_obj_t *sub_lbl = lv_label_create(row);
            lv_obj_set_style_text_font(sub_lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(sub_lbl, C_TEXT_DIM, 0);
            char sub_text[48];
            if (t->running) {
                snprintf(sub_text, sizeof(sub_text), "Running...");
            } else if (t->last_status[0]) {
                snprintf(sub_text, sizeof(sub_text), "%s %s", t->schedule_kind, t->last_status);
            } else {
                snprintf(sub_text, sizeof(sub_text), "%s %s", t->schedule_kind, t->schedule_expr);
            }
            lv_label_set_text(sub_lbl, sub_text);
            lv_obj_set_width(sub_lbl, 88);
            lv_obj_align(sub_lbl, LV_ALIGN_LEFT_MID, 14, 9);

            /* Right side: last status icon */
            lv_obj_t *status_icon = lv_label_create(row);
            lv_obj_set_style_text_font(status_icon, &lv_font_montserrat_14, 0);
            if (t->running) {
                lv_label_set_text(status_icon, LV_SYMBOL_REFRESH);
                lv_obj_set_style_text_color(status_icon, C_ORANGE, 0);
            } else if (strcmp(t->last_status, "ok") == 0) {
                lv_label_set_text(status_icon, LV_SYMBOL_OK);
                lv_obj_set_style_text_color(status_icon, C_GREEN, 0);
            } else if (strcmp(t->last_status, "error") == 0) {
                lv_label_set_text(status_icon, LV_SYMBOL_CLOSE);
                lv_obj_set_style_text_color(status_icon, C_RED, 0);
            } else {
                lv_label_set_text(status_icon, "");
            }
            lv_obj_align(status_icon, LV_ALIGN_RIGHT_MID, -6, 0);
        }
    }

    lvgl_port_unlock();
}

lv_obj_t *ui_tasks_get_screen(void)
{
    return s_tasks_scr;
}

#endif /* BOARD_HAS_DISPLAY */
