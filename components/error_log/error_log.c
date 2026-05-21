/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 */

#include "error_log.h"
#include "cJSON.h"
#include "esp_log.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/time.h>

static error_entry_t s_entries[ERROR_LOG_MAX_ENTRIES];
static int s_head = 0;   /* next write position */
static int s_count = 0;

void error_log_init(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_head = 0;
    s_count = 0;
}

void error_log_add(error_source_t src, error_severity_t sev, const char *fmt, ...)
{
    error_entry_t *e = &s_entries[s_head];

    struct timeval tv;
    gettimeofday(&tv, NULL);
    e->timestamp = tv.tv_sec;
    e->source = src;
    e->severity = sev;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->message, sizeof(e->message), fmt, ap);
    va_end(ap);

    s_head = (s_head + 1) % ERROR_LOG_MAX_ENTRIES;
    if (s_count < ERROR_LOG_MAX_ENTRIES) s_count++;

    /* Also log to ESP_LOG */
    static const char *sev_str[] = {"INFO", "WARN", "ERROR", "CRIT"};
    static const char *src_str[] = {"DEV", "OC", "WIFI", "STT", "TTS"};
    ESP_LOGW("errlog", "[%s][%s] %s",
             src < 5 ? src_str[src] : "?",
             sev < 4 ? sev_str[sev] : "?",
             e->message);
}

int error_log_count(void)
{
    return s_count;
}

const error_entry_t *error_log_get(int index)
{
    if (index < 0 || index >= s_count) return NULL;
    int actual = (s_head - s_count + index + ERROR_LOG_MAX_ENTRIES) % ERROR_LOG_MAX_ENTRIES;
    return &s_entries[actual];
}

char *error_log_to_json(void)
{
    static const char *sev_names[] = {"info", "warning", "error", "critical"};
    static const char *src_names[] = {"device", "openclaw", "wifi", "stt", "tts"};

    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    for (int i = 0; i < s_count; i++) {
        const error_entry_t *e = error_log_get(i);
        if (!e) continue;

        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "ts", (double)e->timestamp);
        cJSON_AddStringToObject(obj, "src", e->source < 5 ? src_names[e->source] : "unknown");
        cJSON_AddStringToObject(obj, "sev", e->severity < 4 ? sev_names[e->severity] : "unknown");
        cJSON_AddStringToObject(obj, "msg", e->message);
        cJSON_AddItemToArray(arr, obj);
    }

    char *str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return str;
}

void error_log_clear(void)
{
    s_head = 0;
    s_count = 0;
}
