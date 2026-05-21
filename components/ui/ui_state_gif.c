/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * UI State GIF Manager Implementation
 */

#include "ui_state_gif.h"
#include "board.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

/* LVGL GIF header - functions are declared when LV_USE_GIF=1 */
#if LV_USE_GIF
#include "extra/libs/gif/lv_gif.h"
#endif

static const char *TAG = "ui_state_gif";

/* GIF file paths on SD card (LVGL FS: S: -> /sdcard/) */
#define GIF_BASE_PATH "S:gifs/"

/* GIF filename mapping */
static const char *gif_filenames[GIF_STATE_COUNT] = {
    [GIF_STATE_SLEEPING]   = GIF_BASE_PATH "sleeping.gif",
    [GIF_STATE_BOOT]       = GIF_BASE_PATH "boot.gif",
    [GIF_STATE_CONNECTING] = GIF_BASE_PATH "connecting.gif",
    [GIF_STATE_IDLE]       = GIF_BASE_PATH "idle.gif",
    [GIF_STATE_LISTENING]  = GIF_BASE_PATH "listening.gif",
    [GIF_STATE_SENDING]    = GIF_BASE_PATH "sending.gif",
    [GIF_STATE_THINKING]   = GIF_BASE_PATH "thinking.gif",
    [GIF_STATE_PLAYING]    = GIF_BASE_PATH "playing.gif",
    [GIF_STATE_RESPONSE]   = GIF_BASE_PATH "response.gif",
    [GIF_STATE_SPEAKING]   = GIF_BASE_PATH "speaking.gif",
    [GIF_STATE_ERROR]      = GIF_BASE_PATH "error.gif",
    [GIF_STATE_NOTIFYING]  = GIF_BASE_PATH "notifying.gif",
};

/* Current GIF object */
static lv_obj_t *s_current_gif = NULL;
static gif_state_type_t s_current_state = GIF_STATE_COUNT;

/* Map UI state to GIF state */
static gif_state_type_t ui_state_to_gif_state(ui_state_t ui_state)
{
    switch (ui_state) {
    case UI_STATE_SLEEP:
    case UI_STATE_ARMED:
        return GIF_STATE_SLEEPING;
    
    case UI_STATE_BOOT:
        return GIF_STATE_BOOT;
    
    case UI_STATE_CONNECTING:
        return GIF_STATE_CONNECTING;
    
    case UI_STATE_IDLE:
        return GIF_STATE_IDLE;
    
    case UI_STATE_LISTENING:
        return GIF_STATE_LISTENING;
    
    case UI_STATE_SENDING:
        return GIF_STATE_SENDING;
    
    case UI_STATE_THINKING:
        return GIF_STATE_THINKING;
    
    case UI_STATE_STREAMING:
        return GIF_STATE_THINKING;  /* STREAMING shares thinking.gif per spec */
    
    case UI_STATE_RESPONSE:
        return GIF_STATE_RESPONSE;
    
    case UI_STATE_TTS_LOADING:
    case UI_STATE_TTS_PLAYING:
        return GIF_STATE_SPEAKING;
    
    case UI_STATE_PLAYING_MP3:
        return GIF_STATE_PLAYING;  /* Use playing.gif for MP3 playback */
    
    case UI_STATE_NOTIFYING:
        return GIF_STATE_NOTIFYING;
    
    case UI_STATE_ERROR:
        return GIF_STATE_ERROR;
    
    default:
        return GIF_STATE_IDLE;
    }
}

esp_err_t ui_state_gif_init(void)
{
    ESP_LOGI(TAG, "Initializing GIF state manager");

    s_current_gif = NULL;
    s_current_state = GIF_STATE_COUNT;
    
    ESP_LOGI(TAG, "GIF state manager initialized");
    return ESP_OK;
}

void ui_state_gif_show_for_state(ui_state_t state)
{
    gif_state_type_t gif_state = ui_state_to_gif_state(state);
    
    /* Only update if state changed */
    if (gif_state == s_current_state && s_current_gif) {
        return;
    }
    
    /* If new state has no GIF configured, just hide current one */
    const char *filepath = gif_filenames[gif_state];
    if (!filepath) {
        ESP_LOGW(TAG, "No GIF configured for state %d, hiding", gif_state);
        ui_state_gif_hide();
        return;
    }
    
    ui_state_gif_show(gif_state);
}

esp_err_t ui_state_gif_show(gif_state_type_t gif_type)
{
    if (gif_type < 0 || gif_type >= GIF_STATE_COUNT) {
        ESP_LOGE(TAG, "Invalid GIF state type: %d", gif_type);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!lvgl_port_lock(500)) {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock");
        return ESP_FAIL;
    }
    
    /* Hide and delete current GIF if exists */
    if (s_current_gif) {
        ESP_LOGD(TAG, "Deleting previous GIF object");
        
        /* Store pointer to local variable and clear global immediately */
        lv_obj_t *old_gif = s_current_gif;
        s_current_gif = NULL;
        s_current_state = GIF_STATE_COUNT;
        
        /* Delete the GIF object - this frees LVGL resources */
        lv_obj_del(old_gif);
        
        /* Force immediate LVGL refresh to clean up */
        lv_refr_now(NULL);
        
        /* Additional memory cleanup */
        lv_task_handler();
        
        ESP_LOGD(TAG, "Previous GIF deleted, running garbage collection");
    }
    
    const char *filepath = gif_filenames[gif_type];
    if (!filepath) {
        ESP_LOGW(TAG, "No GIF configured for state %d", gif_type);
        lvgl_port_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    
    /* Log memory before creating new GIF */
    ESP_LOGD(TAG, "Creating new GIF: %s (Free heap: %d, Free PSRAM: %d)", 
             filepath, 
             esp_get_free_heap_size(),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    /* Check for memory pressure before GIF creation */
    uint32_t free_heap = esp_get_free_heap_size();
    if (free_heap < 100000) {  /* Less than 100KB free */
        ESP_LOGW(TAG, "WARNING: Low memory (%d bytes) before GIF creation: %s",
                 free_heap, filepath);
    }
    
    /* Create GIF object */
    s_current_gif = lv_gif_create(lv_scr_act());
    if (!s_current_gif) {
        ESP_LOGE(TAG, "Failed to create GIF object");
        lvgl_port_unlock();
        return ESP_FAIL;
    }
    
    /* Set GIF source from file */
    lv_gif_set_src(s_current_gif, filepath);
    
    /* Set fixed size: 172x172 pixels */
    lv_obj_set_size(s_current_gif, 172, 172);
    
    /* Position GIF in center of screen */
    lv_obj_center(s_current_gif);
    
    s_current_state = gif_type;
    
    ESP_LOGI(TAG, "Showing GIF for state %d: %s", gif_type, filepath);
    lvgl_port_unlock();
    
    return ESP_OK;
}

void ui_state_gif_hide(void)
{
    if (!lvgl_port_lock(500)) {
        return;
    }
    
    if (s_current_gif) {
        /* Clear global pointer BEFORE deleting to prevent use-after-free */
        lv_obj_t *old_gif = s_current_gif;
        s_current_gif = NULL;
        s_current_state = GIF_STATE_COUNT;
        
        /* Delete the GIF object */
        lv_obj_del(old_gif);
        
        /* Force refresh to ensure cleanup */
        lv_refr_now(NULL);
        lv_task_handler();
        
        ESP_LOGI(TAG, "GIF hidden and cleaned up");
    }
    
    lvgl_port_unlock();
}

lv_obj_t *ui_state_gif_get_current(void)
{
    return s_current_gif;
}
