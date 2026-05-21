/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * Notes Manager implementation
 */

#include "notes_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "notes_manager";

/**
 * Initialize notes manager
 */
esp_err_t notes_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing notes manager");
    
    /* Create notes directory if it doesn't exist */
    struct stat st;
    if (stat(NOTES_DIR, &st) != 0) {
        ESP_LOGI(TAG, "Creating notes directory: %s", NOTES_DIR);
        int ret = mkdir(NOTES_DIR, 0755);
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to create directory: errno=%d", errno);
            return ESP_FAIL;
        }
    }
    
    ESP_LOGI(TAG, "Notes manager initialized");
    return ESP_OK;
}

/**
 * Format current time as ISO 8601 string
 */
void notes_manager_format_timestamp(char *buffer, size_t buffer_size)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(buffer, buffer_size, "%Y-%m-%dT%H:%M:%S", &timeinfo);
}

/**
 * Get today's date as string (YYYY-MM-DD)
 */
void notes_manager_get_today_date(char *buffer, size_t buffer_size)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(buffer, buffer_size, "%Y-%m-%d", &timeinfo);
}

/**
 * Build filename for a given date
 */
static void build_filename(const char *date, char *filename, size_t size)
{
    snprintf(filename, size, "%s/%s%s%s", NOTES_DIR, NOTES_FILE_PREFIX, date, NOTES_FILE_SUFFIX);
}

/**
 * Save a chat message to today's notes file
 */
esp_err_t notes_manager_save_message(const char *role, const char *content, int64_t thinking_time_ms)
{
    if (!role || !content) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Get today's date */
    char date[11];
    notes_manager_get_today_date(date, sizeof(date));
    
    /* Build filename */
    char filename[128];
    build_filename(date, filename, sizeof(filename));
    
    /* Check if file exists */
    FILE *f = fopen(filename, "r");
    bool file_exists = (f != NULL);
    if (f) fclose(f);
    
    /* Build JSON entry string */
    char timestamp[32];
    notes_manager_format_timestamp(timestamp, sizeof(timestamp));
    
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "timestamp", timestamp);
    cJSON_AddStringToObject(entry, "role", role);
    cJSON_AddStringToObject(entry, "content", content);
    if (thinking_time_ms > 0) {
        cJSON_AddNumberToObject(entry, "thinking_time_ms", (double)thinking_time_ms);
    }
    
    char *json_str = cJSON_PrintUnformatted(entry);
    cJSON_Delete(entry);
    if (!json_str) {
        return ESP_FAIL;
    }
    
    if (!file_exists) {
        /* New file: write a complete JSON array with one entry */
        f = fopen(filename, "w");
        if (!f) {
            ESP_LOGE(TAG, "Failed to create file: %s", filename);
            free(json_str);
            return ESP_FAIL;
        }
        fprintf(f, "[\n  %s\n]\n", json_str);
        fclose(f);
        free(json_str);
        ESP_LOGD(TAG, "Saved %s message to %s (new file)", role, filename);
        return ESP_OK;
    }
    
    /* Existing file: open for read+write, re-append entry before trailing ] */
    f = fopen(filename, "r+");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", filename);
        free(json_str);
        return ESP_FAIL;
    }
    
    /* Read the last 2 bytes to detect file format */
    fseek(f, -2, SEEK_END);
    char buf[2] = {0, 0};
    size_t nread = fread(buf, 1, 2, f);
    
    /* Check if file ends with ] (properly closed JSON) or not */
    if (nread == 2 && buf[0] == ']') {
        /* Properly closed: seek back to overwrite the trailing \n] */
        fseek(f, -2, SEEK_END);
        fprintf(f, ",\n  %s\n]\n", json_str);
    } else if (nread == 1 && buf[0] == ']') {
        /* Single byte trail, seek back 1 */
        fseek(f, -1, SEEK_END);
        fprintf(f, ",\n  %s\n]\n", json_str);
    } else {
        /* Old format file (missing closing ]): seek to end and append */
        fseek(f, 0, SEEK_END);
        fprintf(f, ",\n  %s\n]\n", json_str);
    }
    
    fclose(f);
    free(json_str);
    
    ESP_LOGD(TAG, "Saved %s message to %s (append)", role, filename);
    return ESP_OK;
}

/**
 * Get list of available note files
 */
esp_err_t notes_manager_get_file_list(notes_file_info_t ***out_files, int max_files, int *out_count)
{
    if (!out_files || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    DIR *dir = opendir(NOTES_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "Notes directory not found: %s", NOTES_DIR);
        *out_files = NULL;
        *out_count = 0;
        return ESP_OK;
    }
    
    /* Allocate array */
    notes_file_info_t **files = calloc(max_files, sizeof(notes_file_info_t *));
    if (!files) {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }
    
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_files) {
        /* Check if it's a notes file */
        if (strncmp(entry->d_name, NOTES_FILE_PREFIX, strlen(NOTES_FILE_PREFIX)) == 0 &&
            strstr(entry->d_name, NOTES_FILE_SUFFIX) != NULL) {
            
            /* Extract date from filename */
            char *date_start = entry->d_name + strlen(NOTES_FILE_PREFIX);
            char *date_end = strstr(entry->d_name, NOTES_FILE_SUFFIX);
            if (date_end && (date_end - date_start) == 10) {
                notes_file_info_t *info = malloc(sizeof(notes_file_info_t));
                if (!info) continue;
                
                strncpy(info->date, date_start, 10);
                info->date[10] = '\0';
                
                snprintf(info->filename, sizeof(info->filename), "%s/%.200s", NOTES_DIR, entry->d_name);
                
                /* Get file size */
                struct stat st;
                if (stat(info->filename, &st) == 0) {
                    info->file_size = st.st_size;
                } else {
                    info->file_size = 0;
                }
                
                /* Count entries (rough estimate by counting lines with "role") */
                info->entry_count = 0;
                FILE *f = fopen(info->filename, "r");
                if (f) {
                    char line[256];
                    while (fgets(line, sizeof(line), f)) {
                        if (strstr(line, "\"role\"")) {
                            info->entry_count++;
                        }
                    }
                    fclose(f);
                }
                
                files[count++] = info;
            }
        }
    }
    closedir(dir);
    
    *out_files = files;
    *out_count = count;
    
    ESP_LOGI(TAG, "Found %d note files", count);
    return ESP_OK;
}

/**
 * Load all chat entries from a specific date
 */
esp_err_t notes_manager_load_date(const char *date, notes_entry_t ***out_entries, int max_entries, int *out_count)
{
    if (!date || !out_entries || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char filename[128];
    build_filename(date, filename, sizeof(filename));
    
    /* Read entire file */
    FILE *f = fopen(filename, "r");
    if (!f) {
        ESP_LOGW(TAG, "File not found: %s", filename);
        *out_entries = NULL;
        *out_count = 0;
        return ESP_ERR_NOT_FOUND;
    }
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (file_size <= 0) {
        fclose(f);
        *out_entries = NULL;
        *out_count = 0;
        return ESP_OK;
    }
    
    char *json_data = malloc(file_size + 3);
    if (!json_data) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    
    size_t read_size = fread(json_data, 1, file_size, f);
    fclose(f);
    
    /* Handle old-format files (missing trailing \n]) by appending ]
     * Before: [{...}   After: [{...}\n]
     * New-format files end with \n] — leave them as-is. */
    if (read_size > 0 && json_data[read_size - 1] != ']') {
        json_data[read_size] = '\n';
        json_data[read_size + 1] = ']';
        json_data[read_size + 2] = '\0';
    } else {
        json_data[read_size] = '\0';
    }
    
    /* Parse JSON array */
    cJSON *root = cJSON_Parse(json_data);
    free(json_data);
    
    if (!root || !cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "Invalid JSON in file: %s", filename);
        if (root) cJSON_Delete(root);
        return ESP_FAIL;
    }
    
    /* Count entries */
    int total = cJSON_GetArraySize(root);
    int load_count = (total < max_entries) ? total : max_entries;
    
    /* Allocate entries array */
    notes_entry_t **entries = calloc(load_count, sizeof(notes_entry_t *));
    if (!entries) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    
    /* Parse each entry */
    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (idx >= load_count) break;
        
        notes_entry_t *entry = malloc(sizeof(notes_entry_t));
        if (!entry) continue;
        
        memset(entry, 0, sizeof(notes_entry_t));
        
        cJSON *ts = cJSON_GetObjectItem(item, "timestamp");
        if (ts && cJSON_IsString(ts)) {
            strncpy(entry->timestamp, ts->valuestring, sizeof(entry->timestamp) - 1);
        }
        
        cJSON *role = cJSON_GetObjectItem(item, "role");
        if (role && cJSON_IsString(role)) {
            strncpy(entry->role, role->valuestring, sizeof(entry->role) - 1);
        }
        
        cJSON *content = cJSON_GetObjectItem(item, "content");
        if (content && cJSON_IsString(content)) {
            strncpy(entry->content, content->valuestring, sizeof(entry->content) - 1);
        }
        
        cJSON *tt = cJSON_GetObjectItem(item, "thinking_time_ms");
        if (tt && cJSON_IsNumber(tt)) {
            entry->thinking_time_ms = (int64_t)tt->valuedouble;
        }
        
        entries[idx++] = entry;
    }
    
    cJSON_Delete(root);
    
    *out_entries = entries;
    *out_count = idx;
    
    ESP_LOGI(TAG, "Loaded %d entries from %s", idx, date);
    return ESP_OK;
}

/**
 * Load recent chat entries across multiple days
 */
esp_err_t notes_manager_load_recent(int days, notes_entry_t ***out_entries, int max_entries, int *out_count)
{
    if (!out_entries || !out_count || days <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Get file list */
    notes_file_info_t **files = NULL;
    int file_count = 0;
    esp_err_t ret = notes_manager_get_file_list(&files, days * 2, &file_count);
    if (ret != ESP_OK || !files) {
        *out_entries = NULL;
        *out_count = 0;
        return ret;
    }
    
    /* Sort files by date (newest first) - simple bubble sort */
    for (int i = 0; i < file_count - 1; i++) {
        for (int j = 0; j < file_count - i - 1; j++) {
            if (strcmp(files[j]->date, files[j+1]->date) < 0) {
                notes_file_info_t *temp = files[j];
                files[j] = files[j+1];
                files[j+1] = temp;
            }
        }
    }
    
    /* Load entries from most recent days */
    notes_entry_t **all_entries = calloc(max_entries, sizeof(notes_entry_t *));
    if (!all_entries) {
        for (int i = 0; i < file_count; i++) free(files[i]);
        free(files);
        return ESP_ERR_NO_MEM;
    }
    
    int total_count = 0;
    int days_loaded = 0;
    
    for (int i = 0; i < file_count && days_loaded < days && total_count < max_entries; i++) {
        notes_entry_t **day_entries = NULL;
        int day_count = 0;
        
        ret = notes_manager_load_date(files[i]->date, &day_entries, 
                                      max_entries - total_count, &day_count);
        
        if (ret == ESP_OK && day_entries) {
            /* Append to all_entries */
            for (int j = 0; j < day_count && total_count < max_entries; j++) {
                all_entries[total_count++] = day_entries[j];
            }
            free(day_entries);
            days_loaded++;
        }
    }
    
    /* Cleanup file list */
    for (int i = 0; i < file_count; i++) free(files[i]);
    free(files);
    
    *out_entries = all_entries;
    *out_count = total_count;
    
    ESP_LOGI(TAG, "Loaded %d entries from %d days", total_count, days_loaded);
    return ESP_OK;
}

/**
 * Delete a specific date's notes file
 */
esp_err_t notes_manager_delete_date(const char *date)
{
    if (!date) {
        return ESP_ERR_INVALID_ARG;
    }
    
    char filename[128];
    build_filename(date, filename, sizeof(filename));
    
    int ret = unlink(filename);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s (errno=%d)", filename, errno);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Deleted notes for %s", date);
    return ESP_OK;
}

/**
 * Get total storage used by notes
 */
size_t notes_manager_get_storage_used(void)
{
    DIR *dir = opendir(NOTES_DIR);
    if (!dir) {
        return 0;
    }
    
    size_t total = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, NOTES_FILE_PREFIX, strlen(NOTES_FILE_PREFIX)) == 0 &&
            strstr(entry->d_name, NOTES_FILE_SUFFIX) != NULL) {
            
            char filepath[256];
            snprintf(filepath, sizeof(filepath), "%s/%.200s", NOTES_DIR, entry->d_name);
            
            struct stat st;
            if (stat(filepath, &st) == 0) {
                total += st.st_size;
            }
        }
    }
    closedir(dir);
    
    return total;
}

esp_err_t notes_manager_load_for_ai_summary(const char *date, int max_days, char *out_text, size_t text_size)
{
    if (!out_text || text_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    out_text[0] = '\0';
    size_t remaining = text_size - 1; // Reserve for null terminator
    size_t used = 0;
    
    /* Get list of files */
    notes_file_info_t **files = NULL;
    int file_count = 0;
    esp_err_t ret = notes_manager_get_file_list(&files, 100, &file_count);
    if (ret != ESP_OK || file_count == 0) {
        if (files) free(files);
        return ret;
    }
    
    /* Sort by date (newest first) */
    for (int i = 0; i < file_count - 1; i++) {
        for (int j = i + 1; j < file_count; j++) {
            if (strcmp(files[i]->date, files[j]->date) < 0) {
                notes_file_info_t *temp = files[i];
                files[i] = files[j];
                files[j] = temp;
            }
        }
    }
    
    /* Limit to max_days if specified */
    int days_to_load = (max_days > 0 && max_days < file_count) ? max_days : file_count;
    
    /* Load and format messages */
    int loaded_days = 0;
    for (int i = 0; i < file_count && loaded_days < days_to_load; i++) {
        /* If specific date requested, skip others */
        if (date && strcmp(files[i]->date, date) != 0) {
            continue;
        }
        
        /* Load entries for this date */
        notes_entry_t **entries = NULL;
        int entry_count = 0;
        ret = notes_manager_load_date(files[i]->date, &entries, NOTES_MAX_ENTRIES_PER_DAY, &entry_count);
        if (ret != ESP_OK || entry_count == 0) {
            if (entries) free(entries);
            continue;
        }
        
        /* Add date header */
        const char *header_fmt = "\n=== %s ===\n";
        int header_len = snprintf(NULL, 0, header_fmt, files[i]->date);
        if ((size_t)header_len >= remaining) {
            /* Buffer full, stop */
            for (int j = 0; j < entry_count; j++) free(entries[j]);
            free(entries);
            break;
        }
        
        snprintf(out_text + used, remaining, header_fmt, files[i]->date);
        used += header_len;
        remaining -= header_len;
        
        /* Add each message */
        for (int j = 0; j < entry_count; j++) {
            notes_entry_t *entry = entries[j];
            
            /* Format: [timestamp] Role: content */
            const char *role = (strcmp(entry->role, "user") == 0) ? "User" : "Assistant";
            
            /* Extract time from timestamp (ISO 8601: YYYY-MM-DDTHH:MM:SS...) */
            char time_str[9] = {0};
            if (strlen(entry->timestamp) >= 19) {
                strncpy(time_str, entry->timestamp + 11, 8); // HH:MM:SS
                time_str[8] = '\0';
            }
            
            const char *msg_fmt = "[%s] %s: %s\n";
            int msg_len = snprintf(NULL, 0, msg_fmt, time_str, role, entry->content);
            
            if ((size_t)msg_len >= remaining) {
                /* Buffer full, stop */
                break;
            }
            
            snprintf(out_text + used, remaining, msg_fmt, time_str, role, entry->content);
            used += msg_len;
            remaining -= msg_len;
        }
        
        /* Cleanup */
        for (int j = 0; j < entry_count; j++) free(entries[j]);
        free(entries);
        
        loaded_days++;
        
        /* If specific date was requested and found, we're done */
        if (date) {
            break;
        }
    }
    
    /* Cleanup file list */
    for (int i = 0; i < file_count; i++) {
        free(files[i]);
    }
    free(files);

    return ESP_OK;
}

char *notes_manager_read_date(const char *date)
{
    if (!date) return NULL;

    /* Allocate a buffer for formatted chat history text */
    char *buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;

    esp_err_t ret = notes_manager_load_for_ai_summary(date, 1, buf, 16384);
    if (ret != ESP_OK || buf[0] == '\0') {
        free(buf);
        return NULL;
    }

    return buf;
}
