/*
 * SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_chat — 对话编排（Task 7~12）。
 *
 * 完整对话编排：
 *   - doubao_chat_start(): 对话开始入口（DOUBAO_START_BIT，唤醒词/单击共用）
 *   - doubao_chat_on_event(): doubao_init() 注册的事件回调（WSS 任务上下文）
 *   - doubao_chat_tick(): 周期调用（~100ms），VAD 检查 + 超时看门狗 + 打断检测
 *
 * 线程安全：回调在 WSS 任务；tick 在 main loop；UI 调用持 LVGL 锁。
 */

#pragma once

#include <stddef.h>

#include "doubao_voice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 对话开始：置 DOUBAO_START_BIT（唤醒词/单击/say 共用入口）。 */
void doubao_chat_start(void);

/* 播放唤醒反馈音（叮咚），在进入 LISTENING 之前调用。
 * 阻塞直到音效播完（~200ms）。start_listening() 会跳过重复播放。 */
void doubao_chat_play_wake_feedback(void);

/* 直接开始聆听（DOUBAO_START_BIT 消费端）。 */
void doubao_chat_start_listening(void);

/* 取消本轮对话回 IDLE（LISTENING/COMMITTING/THINKING 时单击 BOOT）：
 * go_idle() 清理后播放退出提示音（咚叮），唤醒词恢复。 */
void doubao_chat_cancel(void);

/* doubao 事件回调（doubao_init 注册）。WSS 任务上下文。 */
void doubao_chat_on_event(doubao_event_type_t type, const void *data, size_t len);

/* 周期 tick（~100ms，main loop 调用）：VAD 判停、超时看门狗、打断检测。 */
void doubao_chat_tick(void);

#ifdef __cplusplus
}
#endif
