/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * Error Log — ring buffer for device + OpenClaw errors
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ERR_SRC_DEVICE,
    ERR_SRC_OPENCLAW,
    ERR_SRC_WIFI,
    ERR_SRC_STT,
    ERR_SRC_TTS,
} error_source_t;

typedef enum {
    ERR_SEV_INFO,
    ERR_SEV_WARNING,
    ERR_SEV_ERROR,
    ERR_SEV_CRITICAL,
} error_severity_t;

#define ERROR_LOG_MAX_ENTRIES 32
#define ERROR_LOG_MSG_LEN    128

typedef struct {
    int64_t          timestamp;   /* epoch seconds */
    error_source_t   source;
    error_severity_t severity;
    char             message[ERROR_LOG_MSG_LEN];
} error_entry_t;

/** Initialize error log */
void error_log_init(void);

/** Add an error entry */
void error_log_add(error_source_t src, error_severity_t sev, const char *fmt, ...);

/** Get number of entries currently stored */
int error_log_count(void);

/** Get entry by index (0 = oldest). Returns NULL if out of range. */
const error_entry_t *error_log_get(int index);

/** Export all entries as JSON array string. Caller must free(). */
char *error_log_to_json(void);

/** Clear all entries */
void error_log_clear(void);

#ifdef __cplusplus
}
#endif
