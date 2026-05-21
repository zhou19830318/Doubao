/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * Notes Manager - Chat conversation history storage and retrieval
 * Stores daily chat logs on SD card in /sdcard/notes directory
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOTES_DIR "/sdcard/notes"
#define NOTES_FILE_PREFIX "chat_"
#define NOTES_FILE_SUFFIX ".json"
#define NOTES_MAX_ENTRIES_PER_DAY 100
#define NOTES_MAX_MESSAGE_LEN 4096

/** Chat message entry structure */
typedef struct {
    char timestamp[32];      /* ISO 8601 format: YYYY-MM-DDTHH:MM:SS */
    char role[16];           /* "user" or "assistant" */
    char content[NOTES_MAX_MESSAGE_LEN];
    int64_t thinking_time_ms; /* AI thinking time (for assistant messages) */
} notes_entry_t;

/** Daily notes file info */
typedef struct {
    char date[11];           /* YYYY-MM-DD */
    char filename[256];      /* Full path to file */
    int entry_count;         /* Number of entries in file */
    size_t file_size;        /* File size in bytes */
} notes_file_info_t;

/**
 * Initialize notes manager
 * Creates /sdcard/notes directory if it doesn't exist
 * @return ESP_OK on success
 */
esp_err_t notes_manager_init(void);

/**
 * Save a chat message to today's notes file
 * @param role "user" or "assistant"
 * @param content Message content
 * @param thinking_time_ms AI thinking time (0 for user messages)
 * @return ESP_OK on success
 */
esp_err_t notes_manager_save_message(const char *role, const char *content, int64_t thinking_time_ms);

/**
 * Get list of available note files (dates with chat history)
 * @param out_files Array to store file info pointers
 * @param max_files Maximum number of files to retrieve
 * @param out_count Output: actual number of files found
 * @return ESP_OK on success
 * @note Caller must free each notes_file_info_t* and the array itself
 */
esp_err_t notes_manager_get_file_list(notes_file_info_t ***out_files, int max_files, int *out_count);

/**
 * Load all chat entries from a specific date
 * @param date Date string in YYYY-MM-DD format
 * @param out_entries Array to store entries
 * @param max_entries Maximum entries to load
 * @param out_count Output: actual number of entries loaded
 * @return ESP_OK on success
 * @note Caller must free each entry and the array
 */
esp_err_t notes_manager_load_date(const char *date, notes_entry_t ***out_entries, int max_entries, int *out_count);

/**
 * Load recent chat entries across multiple days
 * @param days Number of recent days to load
 * @param out_entries Array to store entries (newest first)
 * @param max_entries Maximum entries to load
 * @param out_count Output: actual number of entries loaded
 * @return ESP_OK on success
 * @note Caller must free each entry and the array
 */
esp_err_t notes_manager_load_recent(int days, notes_entry_t ***out_entries, int max_entries, int *out_count);

/**
 * Delete a specific date's notes file
 * @param date Date string in YYYY-MM-DD format
 * @return ESP_OK on success
 */
esp_err_t notes_manager_delete_date(const char *date);

/**
 * Get total storage used by notes (in bytes)
 * @return Total bytes used
 */
size_t notes_manager_get_storage_used(void);

/**
 * Format current time as ISO 8601 string
 * @param buffer Output buffer (at least 32 bytes)
 * @param buffer_size Buffer size
 */
void notes_manager_format_timestamp(char *buffer, size_t buffer_size);

/**
 * Get today's date as string (YYYY-MM-DD)
 * @param buffer Output buffer (at least 11 bytes)
 * @param buffer_size Buffer size
 */
void notes_manager_get_today_date(char *buffer, size_t buffer_size);

/**
 * Load chat history for AI summary (returns formatted text)
 * @param date Date string in YYYY-MM-DD format (NULL for all dates)
 * @param max_days Maximum number of days to load (0 for all)
 * @param out_text Output buffer for formatted text
 * @param text_size Size of output buffer
 * @return ESP_OK on success
 * 
 * Format:
 * [2024-01-15 10:30:00] User: Hello
 * [2024-01-15 10:30:05] Assistant: Hi there!
 */
esp_err_t notes_manager_load_for_ai_summary(const char *date, int max_days, char *out_text, size_t text_size);

/**
 * Read chat log for a specific date (YYYY-MM-DD)
 * Returns formatted text buffer (caller must free() the result)
 * @param date Date string in YYYY-MM-DD format
 * @return Malloc'd string with formatted chat log, or NULL if not found/error
 */
char *notes_manager_read_date(const char *date);

#ifdef __cplusplus
}
#endif
