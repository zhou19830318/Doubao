/*
 * SPDX-FileCopyrightText: 2024-2026 AIWatch Contributors
 * SPDX-License-Identifier: MIT
 *
 * 临时测试模块 — Task 12 Step 5 删除（M1 全双工硬件回路验证）
 *
 * Full-duplex I2S loopback test: plays a 440Hz sine on the ES8311 speaker
 * while recording the ES7210 mic on the shared I2S bus (single 16kHz clock),
 * to verify TX+RX stability (underruns / pops) under the real full-duplex
 * clock configuration.
 *
 * 串口命令（由 main/serial_cmd.c 注册）：
 *   audio loop [start|stop]  — 启停全双工回路测试任务
 *   audio rec                — 单次录音 100ms，打印 RMS（mic 电平检查）
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the loopback task (idempotent). Task: 8KB internal stack, pinned core 1. */
esp_err_t board_loopback_start(void);

/** Stop the loopback task (async — waits up to ~500ms for it to exit). */
esp_err_t board_loopback_stop(void);

/** True while the loopback task is alive. */
bool board_loopback_is_running(void);

/** One-shot: record one 100ms block and print RMS / peak stats. */
esp_err_t board_loopback_record_rms(void);

#ifdef __cplusplus
}
#endif
