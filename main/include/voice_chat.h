/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * Voice chat — record audio, STT, send to OpenClaw
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Execute full voice chat flow: start sound → record → stop sound → STT → send */
void voice_chat_start(void);

#ifdef __cplusplus
}
#endif
