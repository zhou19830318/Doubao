/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

// Application version
#define APP_VERSION_MAJOR 0
#define APP_VERSION_MINOR 1
#define APP_VERSION_PATCH 0
#define APP_VERSION_STRING "0.1.0"

// Application name
#define APP_NAME "AIWearable"

// Audio recording buffer size (in samples)
#define APP_AUDIO_RECORD_BUF_SIZE (16000 * 5) // 5 seconds at 16kHz

// Silence detection: stop recording after this many ms of silence
#define APP_SILENCE_TIMEOUT_MS  1200

// Silence threshold: RMS below this is considered silence
#define APP_SILENCE_THRESHOLD   80

// Maximum recording duration (seconds)
#define APP_MAX_RECORD_SECONDS  15

// Speaker volume (0-100, 100 = max)
#define APP_SPEAKER_VOLUME 100

// Inactivity sleep timeout (ms) — 0 to disable
#define APP_SLEEP_TIMEOUT_MS (60 * 1000)
