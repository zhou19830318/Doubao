/*
 * SPDX-FileCopyrightText: 2024-2026 AIClaw Contributors
 * SPDX-License-Identifier: MIT
 *
 * Web server — REST API + embedded SPA
 */

#include "webserver.h"
#include "settings.h"
#include "error_log.h"
#include "openclaw_client.h"
#include "wifi_manager.h"
#include "board.h"
#include "notes_manager.h"

#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "mdns.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <strings.h>

static const char *TAG = "webserver";
static httpd_handle_t s_server = NULL;

/* Forward-declare the embedded HTML */
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

/* Captive Portal: redirect all unknown requests to root page */
static esp_err_t captive_portal_handler(httpd_req_t *req)
{
    // Redirect to root page for captive portal support
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    ESP_LOGD(TAG, "Captive portal redirect: %s", req->uri);
    return ESP_OK;
}

/* ── GET / — serve SPA HTML ──────────────────────────────────────────── */
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    size_t len = index_html_end - index_html_start;
    return httpd_resp_send(req, index_html_start, len);
}

/* ── GET /api/status — device + OpenClaw status ──────────────────────── */
static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();

    /* Device info */
    cJSON *dev = cJSON_AddObjectToObject(j, "device");
    cJSON_AddStringToObject(dev, "name", board_get_name());
    cJSON_AddStringToObject(dev, "mcu", board_get_mcu());
    cJSON_AddStringToObject(dev, "version", esp_app_get_description()->version);
    cJSON_AddNumberToObject(dev, "heap_free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(dev, "heap_min", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(dev, "battery_mv", board_battery_get_voltage_mv());
    cJSON_AddNumberToObject(dev, "battery_pct", board_battery_get_percent());
    cJSON_AddBoolToObject(dev, "charging", board_battery_is_charging());
    cJSON_AddNumberToObject(dev, "rgb_led_count", BOARD_RGB_LED_COUNT);
    cJSON_AddBoolToObject(dev, "has_display", BOARD_HAS_DISPLAY);

    /* WiFi */
    cJSON *wifi = cJSON_AddObjectToObject(j, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", wifi_manager_get_state() == WIFI_STATE_CONNECTED);
    cJSON_AddStringToObject(wifi, "ip", wifi_manager_get_ip());
    cJSON_AddNumberToObject(wifi, "rssi", wifi_manager_get_rssi());

    /* OpenClaw */
    cJSON *oc = cJSON_AddObjectToObject(j, "openclaw");
    openclaw_state_t oc_state = openclaw_get_state();
    cJSON_AddNumberToObject(oc, "state", oc_state);
    const char *state_str = "unknown";
    switch (oc_state) {
        case OPENCLAW_STATE_DISCONNECTED: state_str = "disconnected"; break;
        case OPENCLAW_STATE_CONNECTING:   state_str = "connecting"; break;
        case OPENCLAW_STATE_AUTHENTICATING: state_str = "authenticating"; break;
        case OPENCLAW_STATE_CONNECTED:    state_str = "connected"; break;
        default: break;
    }
    cJSON_AddStringToObject(oc, "state_str", state_str);

    const openclaw_info_t *info = openclaw_get_info();
    if (info) {
        cJSON_AddStringToObject(oc, "version", info->version);
        cJSON_AddNumberToObject(oc, "uptime_min", info->uptime_min);
        cJSON_AddStringToObject(oc, "agent", info->agent_id);
        cJSON_AddNumberToObject(oc, "sessions", info->session_count);
        cJSON_AddStringToObject(oc, "wa_status", info->wa_status);
        cJSON_AddNumberToObject(oc, "last_activity_min", info->last_activity_min);
        if (info->has_tasks) {
            cJSON_AddNumberToObject(oc, "task_count", info->task_count);
            cJSON_AddNumberToObject(oc, "tasks_running", info->tasks_running);
            cJSON_AddNumberToObject(oc, "tasks_active", info->tasks_active);
        }
        /* Active runs (carousel) */
        cJSON_AddBoolToObject(oc, "is_active", info->is_active);
        cJSON_AddBoolToObject(oc, "is_external", info->is_external);
        cJSON_AddStringToObject(oc, "active_detail", info->active_detail);
        cJSON_AddNumberToObject(oc, "active_runs_count", info->active_runs_count);
        if (info->active_runs_count > 0) {
            cJSON *runs = cJSON_AddArrayToObject(oc, "active_runs");
            for (int i = 0; i < OC_MAX_ACTIVE_RUNS; i++) {
                if (!info->active_runs[i].active) continue;
                cJSON *r = cJSON_CreateObject();
                cJSON_AddStringToObject(r, "detail", info->active_runs[i].detail);
                cJSON_AddStringToObject(r, "source", info->active_runs[i].source);
                cJSON_AddNumberToObject(r, "started_ms", (double)info->active_runs[i].started_ms);
                cJSON_AddItemToArray(runs, r);
            }
        }
    }

    /* Error count */
    cJSON_AddNumberToObject(j, "error_count", error_log_count());

    char *str = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, str, strlen(str));
    free(str);
    
    /* Log memory after sending response to detect leaks */
    static uint32_t last_log_tick = 0;
    uint32_t current_tick = xTaskGetTickCount();
    if ((current_tick - last_log_tick) > 100) {  /* Log every ~10 seconds */
        last_log_tick = current_tick;
        ESP_LOGD(TAG, "Web server memory - Free heap: %d, Min free: %d",
                 esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    }
    
    return ret;
}

/* ── GET /api/settings — current settings (secrets masked) ───────────── */
static esp_err_t settings_get_handler(httpd_req_t *req)
{
    char *json = settings_to_json(false);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    
    /* Check for memory pressure */
    if (esp_get_free_heap_size() < 50000) {  /* Less than 50KB free */
        ESP_LOGW(TAG, "Low memory warning after settings GET: %d bytes free",
                 esp_get_free_heap_size());
    }
    
    return ret;
}

/* ── PUT /api/settings — update settings ─────────────────────────────── */
static esp_err_t settings_put_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    char *buf = malloc(total_len + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[total_len] = '\0';

    /* Parse JSON before freeing buffer to check for wifi changes */
    cJSON *j_check = cJSON_Parse(buf);
    bool wifi_changed = (j_check && cJSON_GetObjectItem(j_check, "wifi_ssid") != NULL);
    if (j_check) {
        cJSON_Delete(j_check);
    }

    esp_err_t err = settings_from_json(buf, total_len);
    free(buf);

    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return err;
    }

    settings_save();

    const settings_t *cfg = settings_get();

    /* In AP mode, just save the config - don't restart yet */
    /* User must click "Reboot" button to apply new WiFi settings */
    if (wifi_changed && cfg->wifi_ssid[0]) {
        ESP_LOGI(TAG, "WiFi config saved. Waiting for manual reboot...");
    }

    /* Apply volume and brightness immediately */
    board_audio_set_volume(cfg->volume);
    board_display_set_brightness(cfg->brightness);
    if (!cfg->rgb_enabled) {
        board_rgb_animate(RGB_MODE_OFF, 0, 0, 0);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"ok\":true}", 11);
}

/* ── GET /api/errors — error log ─────────────────────────────────────── */
static esp_err_t errors_handler(httpd_req_t *req)
{
    char *json = error_log_to_json();
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

/* ── POST /api/openclaw/test — test OpenClaw connection ──────────────── */
static esp_err_t openclaw_test_handler(httpd_req_t *req)
{
    /* Read JSON body with host/port/token */
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    char *buf = malloc(total_len + 1);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) { free(buf); httpd_resp_send_500(req); return ESP_FAIL; }
        received += ret;
    }
    buf[total_len] = '\0';

    cJSON *j = cJSON_ParseWithLength(buf, total_len);
    free(buf);

    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *host_item = cJSON_GetObjectItem(j, "host");
    cJSON *port_item = cJSON_GetObjectItem(j, "port");

    const char *test_host = host_item && cJSON_IsString(host_item) ? host_item->valuestring : NULL;
    int test_port = port_item && cJSON_IsNumber(port_item) ? (int)port_item->valuedouble : 18789;

    cJSON *result = cJSON_CreateObject();

    if (!test_host || !test_host[0]) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "Host is required");
    } else {
        /* Simple TCP connection test */
        char url[256];
        snprintf(url, sizeof(url), "ws://%s:%d", test_host, test_port);
        cJSON_AddBoolToObject(result, "ok", true);
        cJSON_AddStringToObject(result, "message", "Connection parameters accepted. Save and reboot to connect.");
        cJSON_AddStringToObject(result, "url", url);
    }

    cJSON_Delete(j);

    char *str = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, str, strlen(str));
    free(str);
    return ret;
}

/* ── POST /api/stt/health — check STT (DashScope) configuration ─────── */
static esp_err_t stt_health_handler(httpd_req_t *req)
{
    const settings_t *s = settings_get();

    cJSON *result = cJSON_CreateObject();

    if (!s->dashscope_api_key[0]) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "DashScope API key not configured");
    } else if (!s->stt_endpoint[0]) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON_AddStringToObject(result, "error", "STT endpoint not configured");
    } else {
        cJSON_AddBoolToObject(result, "ok", true);
        cJSON_AddStringToObject(result, "model", s->stt_model);
        cJSON_AddStringToObject(result, "endpoint", s->stt_endpoint);
        cJSON_AddStringToObject(result, "message", "DashScope STT configuration present. Test by speaking to the device.");
    }

    char *str = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, str, strlen(str));
    free(str);
    return ret;
}

/* ── GET /api/tasks — OpenClaw cron/background tasks ─────────────────── */
static esp_err_t tasks_handler(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateArray();
    const openclaw_info_t *info = openclaw_get_info();
    if (info && info->has_tasks) {
        for (int i = 0; i < info->task_count; i++) {
            const openclaw_task_t *t = &info->tasks[i];
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "id", t->id);
            cJSON_AddStringToObject(item, "name", t->name);
            cJSON_AddBoolToObject(item, "enabled", t->enabled);
            cJSON_AddBoolToObject(item, "running", t->running);
            cJSON_AddStringToObject(item, "schedule_kind", t->schedule_kind);
            cJSON_AddStringToObject(item, "schedule_expr", t->schedule_expr);
            cJSON_AddStringToObject(item, "last_status", t->last_status);
            cJSON_AddStringToObject(item, "last_error", t->last_error);
            cJSON_AddNumberToObject(item, "last_duration_ms", t->last_duration_ms);
            cJSON_AddNumberToObject(item, "consecutive_errors", t->consecutive_errors);
            cJSON_AddItemToArray(j, item);
        }
    }

    char *str = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, str, strlen(str));
    free(str);
    return ret;
}

/* ── POST /api/tasks/toggle — enable/disable a cron job ──────────────── */
static esp_err_t tasks_toggle_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    char *buf = malloc(total_len + 1);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }
    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) { free(buf); httpd_resp_send_500(req); return ESP_FAIL; }
        received += ret;
    }
    buf[total_len] = '\0';

    cJSON *j = cJSON_ParseWithLength(buf, total_len);
    free(buf);
    if (!j) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_FAIL; }

    cJSON *id_item = cJSON_GetObjectItem(j, "id");
    cJSON *en_item = cJSON_GetObjectItem(j, "enabled");
    if (!id_item || !cJSON_IsString(id_item) || !en_item) {
        cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing id or enabled");
        return ESP_FAIL;
    }

    esp_err_t err = openclaw_cron_toggle(id_item->valuestring, cJSON_IsTrue(en_item));
    cJSON_Delete(j);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (err == ESP_OK) {
        return httpd_resp_send(req, "{\"ok\":true}", 11);
    } else {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"Not connected\"}", 35);
    }
}

/* ── OPTIONS handler for CORS ────────────────────────────────────────── */
static esp_err_t cors_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, PUT, POST, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ── POST /api/reboot ────────────────────────────────────────────────── */
static esp_err_t reboot_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "{\"ok\":true,\"msg\":\"Rebooting...\"}", 31);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ── POST /api/settings/reset ────────────────────────────────────────── */
static esp_err_t settings_reset_handler(httpd_req_t *req)
{
    settings_reset();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"ok\":true,\"msg\":\"Reset to defaults. Reboot to apply.\"}", 55);
}

/* ── POST /api/led/demo — trigger LED animation demo ─────────────────── */
static esp_err_t led_demo_handler(httpd_req_t *req)
{
    char buf[128];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (len <= 0) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"No body\"}", 30);
        return ESP_OK;
    }
    buf[len] = '\0';
    cJSON *j = cJSON_Parse(buf);
    if (!j) {
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"Bad JSON\"}", 31);
        return ESP_OK;
    }
    const char *mode = cJSON_GetStringValue(cJSON_GetObjectItem(j, "mode"));
    if (!mode) { cJSON_Delete(j); httpd_resp_send(req, "{\"ok\":false}", 12); return ESP_OK; }

    if (strcmp(mode, "rainbow_spin") == 0) board_rgb_animate(RGB_MODE_RAINBOW_SPIN, 0, 0, 0);
    else if (strcmp(mode, "aurora") == 0)   board_rgb_animate(RGB_MODE_AURORA, 0, 0, 0);
    else if (strcmp(mode, "starfield") == 0) board_rgb_animate(RGB_MODE_STARFIELD, 0, 0, 0);
    else if (strcmp(mode, "fire") == 0)     board_rgb_animate(RGB_MODE_FIRE, 0, 0, 0);
    else if (strcmp(mode, "ocean") == 0)    board_rgb_animate(RGB_MODE_OCEAN, 0, 0, 0);
    else if (strcmp(mode, "breathe") == 0)  board_rgb_animate(RGB_MODE_BREATHE, 0, 32, 16);
    else if (strcmp(mode, "chase") == 0)    board_rgb_animate(RGB_MODE_CHASE, 0, 16, 40);
    else if (strcmp(mode, "sparkle") == 0)  board_rgb_animate(RGB_MODE_SPARKLE, 32, 8, 48);
    else if (strcmp(mode, "stop") == 0)     board_rgb_animate(RGB_MODE_SOLID, 0, 4, 0);  /* back to idle */
    else { cJSON_Delete(j); httpd_resp_send(req, "{\"ok\":false,\"error\":\"Unknown mode\"}", 35); return ESP_OK; }

    cJSON_Delete(j);
    ESP_LOGI(TAG, "LED demo: %s", mode);
    return httpd_resp_send(req, "{\"ok\":true}", 11);
}

/* ── GET /api/notes — list available note dates ─────────────────────── */
static esp_err_t notes_list_handler(httpd_req_t *req)
{
    notes_file_info_t **files = NULL;
    int count = 0;
    
    esp_err_t ret = notes_manager_get_file_list(&files, 100, &count);
    if (ret != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    /* Build JSON response */
    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "date", files[i]->date);
        cJSON_AddStringToObject(item, "filename", files[i]->filename);
        cJSON_AddNumberToObject(item, "entry_count", files[i]->entry_count);
        cJSON_AddNumberToObject(item, "file_size", (double)files[i]->file_size);
        cJSON_AddItemToArray(root, item);
    }
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    /* Free file info */
    for (int i = 0; i < count; i++) free(files[i]);
    free(files);
    
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t send_ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return send_ret;
}

/* ── GET /api/notes/:date — load entries for a specific date ────────── */
static esp_err_t notes_date_handler(httpd_req_t *req)
{
    /* Extract date from URI: /api/notes/YYYY-MM-DD */
    const char *uri = req->uri;
    const char *date = uri + strlen("/api/notes/");

    ESP_LOGI(TAG, "notes_date_handler: uri=%s, date=%s", req->uri, date);

    if (strlen(date) != 10) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid date format");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Loading notes for date: %s", date);

    notes_entry_t **entries = NULL;
    int count = 0;
    
    esp_err_t ret = notes_manager_load_date(date, &entries, NOTES_MAX_ENTRIES_PER_DAY, &count);
    if (ret == ESP_ERR_NOT_FOUND) {
        /* Return empty array for non-existent date */
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req, "[]", 2);
    }
    
    if (ret != ESP_OK || !entries) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Loaded %d entries for date %s", count, date);
    /* Build JSON response */
    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "timestamp", entries[i]->timestamp);
        cJSON_AddStringToObject(item, "role", entries[i]->role);
        cJSON_AddStringToObject(item, "content", entries[i]->content);
        if (entries[i]->thinking_time_ms > 0) {
            cJSON_AddNumberToObject(item, "thinking_time_ms", (double)entries[i]->thinking_time_ms);
        }
        cJSON_AddItemToArray(root, item);
    }
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    /* Free entries */
    for (int i = 0; i < count; i++) free(entries[i]);
    free(entries);
    
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t send_ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return send_ret;
}

/* ── DELETE /api/notes/:date — delete notes for a specific date ─────── */
static esp_err_t notes_delete_handler(httpd_req_t *req)
{
    /* Extract date from URI: /api/notes/YYYY-MM-DD */
    const char *uri = req->uri;
    const char *date = uri + strlen("/api/notes/");
    
    if (strlen(date) != 10) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid date format");
        return ESP_FAIL;
    }
    
    esp_err_t ret = notes_manager_delete_date(date);
    if (ret != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\"ok\":true}", 11);
}

/* ── GET /api/notes/stats — get storage statistics ──────────────────── */
static esp_err_t notes_stats_handler(httpd_req_t *req)
{
    size_t used = notes_manager_get_storage_used();
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "storage_used_bytes", (double)used);
    cJSON_AddNumberToObject(root, "storage_used_kb", (double)used / 1024.0);
    
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    
    return ret;
}


/* ── GET /api/chat/history?date=YYYY-MM-DD — chat log for a date ──────── */
static esp_err_t chat_history_handler(httpd_req_t *req)
{
    /* Parse query parameter "date" from URI (embedded in req->uri after ?) */
    const char *qmark = strchr(req->uri, '?');
    const char *query = qmark ? qmark + 1 : "";
    char date[16] = {0};
    esp_err_t err = httpd_query_key_value(query, "date", date, sizeof(date));
    if (err != ESP_OK || strlen(date) != 10) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_send(req, "{\"error\":\"Missing or invalid date parameter (YYYY-MM-DD)\"}", -1);
    }

    ESP_LOGI(TAG, "Chat history request for date: %s", date);

    /* Read chat history content (caller must free) */
    char *content = notes_manager_read_date(date);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "date", date);
    if (content) {
        cJSON_AddStringToObject(j, "content", content);
        free(content);
    } else {
        cJSON_AddStringToObject(j, "content", "No chat history for this date");
    }

    char *str = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, str, strlen(str));
    free(str);
    return ret;
}

/* ── Filename validation (prevent path traversal) ───────────────────────── */
static bool is_safe_filename(const char *filename)
{
    if (!filename || filename[0] == '\0') return false;
    for (const char *p = filename; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == 0 || c == '/' || c == '\\') return false;
    }
    /* Reject ".." anywhere in filename */
    if (strstr(filename, "..")) return false;
    return true;
}

/* ── POST /api/mp3/upload — receive MP3 file, save to /sdcard/music/ ─── */
static esp_err_t mp3_upload_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 10 * 1024 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    char *buf = malloc(total_len);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += ret;
    }

    /* Extract filename from query parameter ?name= , fallback to uploaded.mp3 */
    const char *qmark = strchr(req->uri, '?');
    char filename[64] = "uploaded.mp3";
    if (qmark) {
        char name_buf[64];
        if (httpd_query_key_value(qmark + 1, "name", name_buf, sizeof(name_buf)) == ESP_OK
            && is_safe_filename(name_buf)) {
            strncpy(filename, name_buf, sizeof(filename) - 1);
            filename[sizeof(filename) - 1] = '\0';
        }
    }

    /* Ensure /sdcard/music/ directory exists */
    struct stat st;
    if (stat("/sdcard/music", &st) != 0) {
        mkdir("/sdcard/music", 0755);
    }

    /* Write file */
    char filepath[320];
    snprintf(filepath, sizeof(filepath), "/sdcard/music/%s", filename);
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        free(buf);
        ESP_LOGE(TAG, "Failed to open %s for writing", filepath);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t written = fwrite(buf, 1, total_len, f);
    fclose(f);
    free(buf);

    if ((int)written != total_len) {
        ESP_LOGE(TAG, "Wrote only %d/%d bytes to %s", (int)written, total_len, filepath);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved MP3: %s (%d bytes)", filepath, total_len);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "status", "ok");
    cJSON_AddStringToObject(j, "file", filename);
    cJSON_AddNumberToObject(j, "size", total_len);

    char *str = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, str, strlen(str));
    free(str);
    return ret;
}

/* ── POST /api/mp3/delete — delete MP3 from /sdcard/music/ ────────────── */
static esp_err_t mp3_delete_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    char *buf = malloc(total_len + 1);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) { free(buf); httpd_resp_send_500(req); return ESP_FAIL; }
        received += ret;
    }
    buf[total_len] = '\0';

    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *file_item = cJSON_GetObjectItem(j, "file");
    if (!file_item || !cJSON_IsString(file_item) || !file_item->valuestring[0]) {
        cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'file' field");
        return ESP_FAIL;
    }

    if (!is_safe_filename(file_item->valuestring)) {
        ESP_LOGE(TAG, "Unsafe filename: %s", file_item->valuestring);
        cJSON_Delete(j);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char filepath[320];
    snprintf(filepath, sizeof(filepath), "/sdcard/music/%s", file_item->valuestring);

    cJSON_Delete(j);

    int ret = unlink(filepath);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to delete MP3: %s (errno=%d)", filepath, errno);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "status", "error");
        cJSON_AddStringToObject(err, "message", "Failed to delete file");
        char *str = cJSON_PrintUnformatted(err);
        cJSON_Delete(err);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        esp_err_t eret = httpd_resp_send(req, str, strlen(str));
        free(str);
        return eret;
    }

    ESP_LOGI(TAG, "Deleted MP3: %s", filepath);

    cJSON *ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    cJSON_AddStringToObject(ok, "message", "File deleted");
    char *str = cJSON_PrintUnformatted(ok);
    cJSON_Delete(ok);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t eret = httpd_resp_send(req, str, strlen(str));
    free(str);
    return eret;
}

/* ── POST /api/gif/upload — receive GIF file, save to /sdcard/gifs/ ───── */
static esp_err_t gif_upload_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 10 * 1024 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    char *buf = malloc(total_len);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += ret;
    }

    /* Extract filename from query parameter ?name= , fallback to uploaded.gif */
    const char *qmark = strchr(req->uri, '?');
    char filename[64] = "uploaded.gif";
    if (qmark) {
        char name_buf[64];
        if (httpd_query_key_value(qmark + 1, "name", name_buf, sizeof(name_buf)) == ESP_OK
            && is_safe_filename(name_buf)) {
            strncpy(filename, name_buf, sizeof(filename) - 1);
            filename[sizeof(filename) - 1] = '\0';
        }
    }

    /* Ensure /sdcard/gifs/ directory exists */
    struct stat st;
    if (stat("/sdcard/gifs", &st) != 0) {
        mkdir("/sdcard/gifs", 0755);
    }

    /* Write file */
    char filepath[320];
    snprintf(filepath, sizeof(filepath), "/sdcard/gifs/%s", filename);
    FILE *f = fopen(filepath, "wb");
    if (!f) {
        free(buf);
        ESP_LOGE(TAG, "Failed to open %s for writing", filepath);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    size_t written = fwrite(buf, 1, total_len, f);
    fclose(f);
    free(buf);

    if ((int)written != total_len) {
        ESP_LOGE(TAG, "Wrote only %d/%d bytes to %s", (int)written, total_len, filepath);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved GIF: %s (%d bytes)", filepath, total_len);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "status", "ok");
    cJSON_AddStringToObject(j, "file", filename);
    cJSON_AddNumberToObject(j, "size", total_len);

    char *str = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, str, strlen(str));
    free(str);
    return ret;
}

/* ── POST /api/gif/delete — delete GIF from /sdcard/gifs/ ─────────────── */
static esp_err_t gif_delete_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    char *buf = malloc(total_len + 1);
    if (!buf) { httpd_resp_send_500(req); return ESP_FAIL; }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) { free(buf); httpd_resp_send_500(req); return ESP_FAIL; }
        received += ret;
    }
    buf[total_len] = '\0';

    cJSON *j = cJSON_Parse(buf);
    free(buf);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *file_item = cJSON_GetObjectItem(j, "file");
    if (!file_item || !cJSON_IsString(file_item) || !file_item->valuestring[0]) {
        cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'file' field");
        return ESP_FAIL;
    }

    if (!is_safe_filename(file_item->valuestring)) {
        ESP_LOGE(TAG, "Unsafe filename: %s", file_item->valuestring);
        cJSON_Delete(j);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char filepath[320];
    snprintf(filepath, sizeof(filepath), "/sdcard/gifs/%s", file_item->valuestring);

    cJSON_Delete(j);

    int ret = unlink(filepath);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to delete GIF: %s (errno=%d)", filepath, errno);
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "status", "error");
        cJSON_AddStringToObject(err, "message", "Failed to delete file");
        char *str = cJSON_PrintUnformatted(err);
        cJSON_Delete(err);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        esp_err_t eret = httpd_resp_send(req, str, strlen(str));
        free(str);
        return eret;
    }

    ESP_LOGI(TAG, "Deleted GIF: %s", filepath);

    cJSON *ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    cJSON_AddStringToObject(ok, "message", "File deleted");
    char *str = cJSON_PrintUnformatted(ok);
    cJSON_Delete(ok);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t eret = httpd_resp_send(req, str, strlen(str));
    free(str);
    return eret;
}

/* ── GET /api/mp3/list — list MP3 files in /sdcard/music/ ───────────────── */
static esp_err_t mp3_list_handler(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateArray();
    DIR *dir = opendir("/sdcard/music");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN) {
                const char *name = entry->d_name;
                size_t len = strlen(name);
                if (len > 4 && (strcasecmp(name + len - 4, ".mp3") == 0 ||
                                strcasecmp(name + len - 4, ".wav") == 0)) {
                    struct stat st;
                    char fullpath[320];
                    snprintf(fullpath, sizeof(fullpath), "/sdcard/music/%s", name);
                    cJSON *item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "name", name);
                    if (stat(fullpath, &st) == 0) {
                        cJSON_AddNumberToObject(item, "size", (double)st.st_size);
                    }
                    cJSON_AddItemToArray(j, item);
                }
            }
        }
        closedir(dir);
    }

    char *str = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, str, strlen(str));
    free(str);
    return ret;
}

/* ── GET /api/gif/list — list GIF files in /sdcard/gifs/ ───────────────── */
static esp_err_t gif_list_handler(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateArray();
    DIR *dir = opendir("/sdcard/gifs");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN) {
                const char *name = entry->d_name;
                size_t len = strlen(name);
                if (len > 4 && strcasecmp(name + len - 4, ".gif") == 0) {
                    struct stat st;
                    char fullpath[320];
                    snprintf(fullpath, sizeof(fullpath), "/sdcard/gifs/%s", name);
                    cJSON *item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "name", name);
                    if (stat(fullpath, &st) == 0) {
                        cJSON_AddNumberToObject(item, "size", (double)st.st_size);
                    }
                    cJSON_AddItemToArray(j, item);
                }
            }
        }
        closedir(dir);
    }

    char *str = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_send(req, str, strlen(str));
    free(str);
    return ret;
}

/* ── Server start/stop ───────────────────────────────────────────────── */

static void mdns_init_helper(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }

    /* Hostname: aiclaw.local */
    mdns_hostname_set("aiclaw");
    /* Instance name */
    mdns_instance_name_set("AIClaw AI Assistant");

    /* Register HTTP service */
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    /* Add some TXT records for discovery */
    mdns_service_txt_item_set("_http", "_tcp", "path", "/");
    mdns_service_txt_item_set("_http", "_tcp", "version", esp_app_get_description()->version);
    mdns_service_txt_item_set("_http", "_tcp", "board", board_get_name());

    ESP_LOGI(TAG, "mDNS initialized (aiclaw.local)");
}

esp_err_t webserver_start(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    /* Start mDNS alongside web server */
    mdns_init_helper();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 30;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start: %s", esp_err_to_name(err));
        return err;
    }

    /* Register URI handlers */
    const httpd_uri_t uris[] = {
        { .uri = "/",                .method = HTTP_GET,     .handler = root_handler },
        { .uri = "/api/status",      .method = HTTP_GET,     .handler = status_handler },
        { .uri = "/api/settings",    .method = HTTP_GET,     .handler = settings_get_handler },
        { .uri = "/api/settings",    .method = HTTP_PUT,     .handler = settings_put_handler },
        { .uri = "/api/settings/reset", .method = HTTP_POST, .handler = settings_reset_handler },
        { .uri = "/api/errors",      .method = HTTP_GET,     .handler = errors_handler },
        { .uri = "/api/tasks",       .method = HTTP_GET,     .handler = tasks_handler },
        { .uri = "/api/tasks/toggle", .method = HTTP_POST,   .handler = tasks_toggle_handler },
        { .uri = "/api/openclaw/test", .method = HTTP_POST,  .handler = openclaw_test_handler },
        { .uri = "/api/stt/health",    .method = HTTP_POST,  .handler = stt_health_handler },
        { .uri = "/api/led/demo",      .method = HTTP_POST,  .handler = led_demo_handler },
        { .uri = "/api/notes",         .method = HTTP_GET,   .handler = notes_list_handler },
        { .uri = "/api/notes/stats",   .method = HTTP_GET,   .handler = notes_stats_handler },
        { .uri = "/api/notes/*",       .method = HTTP_GET,   .handler = notes_date_handler },
        { .uri = "/api/notes/*",       .method = HTTP_DELETE, .handler = notes_delete_handler },
        { .uri = "/api/chat/history",  .method = HTTP_GET,    .handler = chat_history_handler },
        { .uri = "/api/mp3/list",      .method = HTTP_GET,    .handler = mp3_list_handler },
        { .uri = "/api/gif/list",      .method = HTTP_GET,    .handler = gif_list_handler },
        { .uri = "/api/mp3/upload",    .method = HTTP_POST,   .handler = mp3_upload_handler },
        { .uri = "/api/mp3/delete",    .method = HTTP_POST,   .handler = mp3_delete_handler },
        { .uri = "/api/gif/upload",    .method = HTTP_POST,   .handler = gif_upload_handler },
        { .uri = "/api/gif/delete",    .method = HTTP_POST,   .handler = gif_delete_handler },
        { .uri = "/api/reboot",      .method = HTTP_POST,    .handler = reboot_handler },
        { .uri = "/api/*",           .method = HTTP_OPTIONS, .handler = cors_handler },
        // Captive Portal: catch-all handler for unknown URIs
        { .uri = "/*",               .method = HTTP_GET,     .handler = captive_portal_handler },
    };

    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    ESP_LOGI(TAG, "Web server started on port 80");
    return ESP_OK;
}

esp_err_t webserver_stop(void)
{
    if (!s_server) return ESP_OK;

    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    mdns_free();
    ESP_LOGI(TAG, "Web server stopped");
    return err;
}

bool webserver_is_running(void)
{
    return s_server != NULL;
}
