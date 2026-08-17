/*
 * SPDX-FileCopyrightText: 2024-2026 AIWatch Contributors
 * SPDX-License-Identifier: MIT
 *
 * doubao_chat — 对话编排雏形（Task 7：文本链路 E2E）。
 *
 * 本任务只做文本链路：
 *   - doubao_chat_on_event(): doubao_init() 注册的事件回调（WSS 任务上下文），
 *     把 DOUBAO_EVT_* 分发到 UI 气泡 / 状态机 / [DEVICE:] 指令 / notes 落盘。
 *   - doubao_chat_start(): 对话开始入口（DOUBAO_START_BIT 事件位触发，
 *     say 命令/唤醒词共用；本任务只接 say，唤醒词在 Task 10 接线）。
 *
 * 铁律：回调 data 指向协议层内部缓冲，回调返回即失效 —— 必须回调内拷贝；
 * 非 LVGL 任务调 UI 必须 lvgl_port_lock()/unlock() 包裹。
 */

#pragma once

#include <stddef.h>

#include "doubao_voice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 对话开始：置 DOUBAO_START_BIT（Task 10 唤醒词共用入口；本任务 say 命令直接
 * 走 doubao_push_text，不依赖此函数）。 */
void doubao_chat_start(void);

/* doubao 事件回调（doubao_init 注册）。WSS 任务上下文，保持轻量：
 * 数据须在回调内拷贝；UI 调用须持 LVGL 锁。 */
void doubao_chat_on_event(doubao_event_type_t type, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
