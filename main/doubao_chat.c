/*
 * SPDX-FileCopyrightText: 2024-2026 AIWatch Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_chat — 对话编排（Task 7：文本链路 E2E 雏形）。
 *
 * 事件流（say 命令链路）：
 *   serial_cmd: say <文本> → ui_add_user_bubble + SENDING →
 *   doubao_push_text（replacement.append + commit）→
 *   服务端 response.output_text.delta ×N → ui_bot_bubble_append（流式）+
 *   STREAMING → response.output_text.done → notes 落盘 + [DEVICE:] 指令解析
 *   + 状态置 IDLE。
 *
 * 线程：回调运行在 WSS 任务上下文；UI 调用一律 lvgl_port_lock/unlock 包裹；
 * SD/NVS 等慢操作不持锁。回调 data 生命周期：协议层内部缓冲，回调内即拷贝。
 */

#include "doubao_chat.h"

#include "app_state.h"
#include "app_state_machine.h"
#include "ui.h"
#include "error_log.h"
#include "notes_manager.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

static const char *TAG = "doubao_chat";

/* OUTPUT_TEXT_DONE 的完整回复：回调内拷贝（proto 缓冲回调后失效） */
static char s_final_text[NOTES_MAX_MESSAGE_LEN];

/* 本回合是否已有流式 delta 上屏（DONE 时若无 delta 则补整段文本） */
static bool s_bot_has_text = false;

void doubao_chat_start(void)
{
    /* DOUBAO_START_BIT 事件组入口（say/唤醒共用）；本任务只接 say，
     * 位在 app_state.h 定义，Task 10 唤醒词消费。 */
    if (g_app_events) {
        xEventGroupSetBits(g_app_events, DOUBAO_START_BIT);
        ESP_LOGD(TAG, "DOUBAO_START_BIT set");
    }
}

void doubao_chat_on_event(doubao_event_type_t type, const void *data, size_t len)
{
    (void)len;

    switch (type) {

    case DOUBAO_EVT_SESSION_CREATED:
        ESP_LOGI(TAG, "session created: id=%s",
                 doubao_get_session_id() ? doubao_get_session_id() : "(none)");
        break;

    case DOUBAO_EVT_CONNECTED:
        ESP_LOGI(TAG, "WSS connected");
        break;

    case DOUBAO_EVT_DISCONNECTED:
        ESP_LOGW(TAG, "WSS disconnected");
        break;

    case DOUBAO_EVT_TRANSCRIPT_DELTA:
    case DOUBAO_EVT_TRANSCRIPT_DONE:
        /* 用户识别文本 → 用户气泡（DONE 携带完整文本，整体替换） */
        if (data) {
            lvgl_port_lock(0);
            ui_add_user_bubble((const char *)data);
            lvgl_port_unlock();
        }
        break;

    case DOUBAO_EVT_OUTPUT_TEXT_DELTA:
        /* 回复文本流式 → 机器人气泡追加 */
        if (data) {
            lvgl_port_lock(0);
            if (ui_get_state() != UI_STATE_STREAMING) {
                app_set_state(UI_STATE_STREAMING);
            }
            ui_bot_bubble_append((const char *)data);
            lvgl_port_unlock();
            s_bot_has_text = true;
        }
        break;

    case DOUBAO_EVT_OUTPUT_TEXT_DONE: {
        if (!data) break;
        /* 1. 回调内拷贝（proto 缓冲回调后失效） */
        strncpy(s_final_text, (const char *)data, sizeof(s_final_text) - 1);
        s_final_text[sizeof(s_final_text) - 1] = '\0';
        ESP_LOGI(TAG, "reply done: %.100s%s", s_final_text,
                 strlen(s_final_text) > 100 ? "..." : "");

        /* 2. 对话记录落盘（SD 慢操作，不持 LVGL 锁）——Ver2.0 on_chat_response 移植 */
        esp_err_t ret = notes_manager_save_message("assistant", s_final_text, 0);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "notes save failed: %s", esp_err_to_name(ret));
        }

        /* 3. [DEVICE:] 语音控制解析（Ver2.0 parse_device_commands 移植；
         * 可能在内部 vTaskDelay/esp_restart，绝不能持锁） */
        char *cmd_copy = strdup(s_final_text);
        if (cmd_copy) {
            int n = parse_device_commands(cmd_copy);
            if (n > 0) {
                ESP_LOGI(TAG, "Executed %d device command(s)", n);
            }
            free(cmd_copy);
        }

        /* 4. 收尾：无 delta 的极端情况补整段文本；状态回 IDLE。
         * 文本链路用 app_set_state 直改 UI（机器上 IDLE→SENDING 非合法
         * 迁移，Ver2.0 voice_chat 同模式）；若机器已漂移（如 say 在
         * LISTENING 时发出），用 app_state_request(IDLE) 同步机器。 */
        lvgl_port_lock(0);
        if (!s_bot_has_text) {
            ui_bot_bubble_append(s_final_text);
        }
        if (app_state_current() == UI_STATE_IDLE) {
            app_set_state(UI_STATE_IDLE);
        } else {
            app_state_request(UI_STATE_IDLE);
        }
        lvgl_port_unlock();
        s_bot_has_text = false;
        break;
    }

    case DOUBAO_EVT_ERROR: {
        const char *msg = data ? (const char *)data : "unknown error";
        ESP_LOGE(TAG, "Doubao error: %s", msg);
        error_log_add(ERR_SRC_DEVICE, ERR_SEV_ERROR, "Doubao: %.100s", msg);
        lvgl_port_lock(0);
        app_set_state(UI_STATE_ERROR);
        lvgl_port_unlock();
        break;
    }

    /* 音频链路事件：Task 9（播报/打断）处理，本任务仅留痕 */
    case DOUBAO_EVT_AUDIO_STARTED:
    case DOUBAO_EVT_AUDIO_DELTA:
    case DOUBAO_EVT_AUDIO_DONE:
    case DOUBAO_EVT_RESPONSE_DONE:
    case DOUBAO_EVT_INTERRUPTED:
        ESP_LOGD(TAG, "audio-chain event %d (handled in Task 9)", (int)type);
        break;

    default:
        ESP_LOGD(TAG, "unhandled event type %d", (int)type);
        break;
    }
}
