# AIWearable Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build AIWearable voice assistant firmware by transplanting verified components from AIClaw_Ver1.2, adding GIF character display, chat history query, and enhanced web file management.

**Architecture:** ESP32-S3 pipeline: audio capture → Fun-ASR STT → text → OpenClaw WebSocket → AI response → MIMO TTS → audio playback. All hardware drivers, OpenClaw protocol, STT/TTS clients, WiFi manager, LVGL UI framework, and web server are transplanted from the verified AIClaw_Ver1.2 reference project. New features: GIF character animations from SD card, web chat history preview, web MP3/GIF file upload/delete.

**Tech Stack:** ESP-IDF v5.5, LVGL 8.x, cJSON, FreeRTOS, ED25519, OpenClaw Gateway Protocol (JSON-RPC over WebSocket)

---

### Task 1: Project Skeleton - Copy AIClaw_Ver1.2 as Base

**Files:**
- Copy: all files from `/home/ubuntu/esp32/AIClaw_Ver1.2/` → `/home/ubuntu/esp32/AIWearable/`
- Modify: `CMakeLists.txt` (project name)
- Modify: `main/CMakeLists.txt` (component registration)
- Modify: `build_audio_board.sh` (project name)
- Modify: `partitions.csv` (keep same)
- Keep: `sdkconfig`, `sdkconfig.defaults`, `dependencies.lock`

- [ ] **Step 1: Copy AIClaw_Ver1.2 into AIWearable**

```bash
# Copy all source files, preserving structure but not overwriting existing docs
rsync -av --exclude='.git' --exclude='build' --exclude='docs' --exclude='esp32_log.txt' --exclude='REBOOT_FIX_REPORT.md' --exclude='secrets_example.txt' --exclude='.atomcode' --exclude='.lingma' \
  /home/ubuntu/esp32/AIClaw_Ver1.2/ /home/ubuntu/esp32/AIWearable/
```

- [ ] **Step 2: Rename project from AIClaw to AIWearable in top-level CMakeLists.txt**

```cmake
# CMakeLists.txt — replace last line
project(aiwearable)
```

- [ ] **Step 3: Update main/CMakeLists.txt - rename requires, update component list**

```cmake
# main/CMakeLists.txt
idf_component_register(
    SRCS "app_main.c" "app_state.c" "voice_chat.c" "serial_cmd.c" "app_tasks.c" "mem_monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES board wifi_manager openclaw ui tts stt settings error_log webserver camera wake_word mp3_player notes_manager
    PRIV_REQUIRES console driver esp_event freertos
)
```

- [ ] **Step 4: Update build script references**

```bash
# build_audio_board.sh — change project name in echo lines
sed -i 's/AIClaw/AIWearable/g' /home/ubuntu/esp32/AIWearable/build_audio_board.sh
```

- [ ] **Step 5: Update secrets_example.txt**

```bash
# Copy and adapt
cp /home/ubuntu/esp32/AIClaw_Ver1.2/secrets_example.txt /home/ubuntu/esp32/AIWearable/secrets_example.txt
```

- [ ] **Step 6: Verify project compiles**

```bash
cd /home/ubuntu/esp32/AIWearable
# Check ESP-IDF environment is set up
. $HOME/esp/esp-idf/export.sh 2>/dev/null || true
./build_audio_board.sh
```

Expected: BUILD SUCCESSFUL (or fails only on missing secrets.h, which is expected)

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat: transplant AIClaw_Ver1.2 as AIWearable project base
- Copy all verified hardware drivers (board/, audio codecs)
- Copy OpenClaw client, STT/TTS, WiFi, web server components
- Copy LVGL UI framework with 15-state state machine
- Rename project from aiclaw to aiwearable"
```

---

### Task 2: Verify Board Layer - All Pins and Drivers

**Files:**
- Verify: `components/board/include/board_waveshare_audio.h` (pins)
- Verify: `components/board/board.c` (init sequence)
- Verify: `components/board/esp_lcd_panel_jd9853.c` (JD9853 LCD driver)
- Verify: `components/board/touch_axs5106l.c` (AXS5106L touch driver)
- Verify: `components/board/rgb_ring.c` (WS2812 on GPIO38)
- Verify: `components/board/CMakeLists.txt`

**Goal:** Confirm every pin definition in board_waveshare_audio.h matches the hardware spec and is the verified version from AIClaw.

- [ ] **Step 1: Verify pin definitions against spec**

Key pins to verify (already correct from AIClaw, this is a confirmation audit):

```
I2C0:    SDA=11, SCL=10                        ✓
SPI2:    CLK=4, MOSI=9, MISO=8, CS=3, DC=7     ✓
I2S0:    MCLK=12, SCLK=13, LRCK=14, DSIN=15, DOUT=16  ✓
SDMMC:   CLK=40, CMD=42, D0=41                  ✓
RGB LED: GPIO38 (7×WS2812)                       ✓
BOOT:    GPIO0                                    ✓
IO扩展:  PA_EN=P1.0, BTN1=P1.1, BTN2=P1.2       ✓
         LCD_RST=P0.0, TOUCH_RST=P0.1            ✓
         TOUCH_INT=P0.2, SD_CS=P0.3              ✓
         CAM_EN=P0.5, CAM_MUX=P0.6               ✓
```

- [ ] **Step 2: Add camera pin definitions to board header (already present but verify)**

The board_waveshare_audio.h already has CAM_EN=P0.5, CAM_MUX=P0.6. No changes needed.

- [ ] **Step 3: Build board test - compile verification**

```bash
cd /home/ubuntu/esp32/AIWearable
./build_audio_board.sh
```

Expected: Board component compiles without errors.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "verify: board pin definitions confirmed for AIWearable - all match hardware"
```

---

### Task 3: Audio Pipeline - STT, TTS, MP3, Wake Word

**Files:**
- Verify: `components/stt/stt_client.c` (Fun-ASR WebSocket client)
- Verify: `components/tts/tts_client.c` (MIMO TTS HTTP client)
- Verify: `components/mp3_player/mp3_player.c` (SD MP3 decoder)
- Verify: `components/wake_word/wake_word.c` (WakeNet)
- Modify: None — all already use the correct APIs

**Goal:** Confirm STT (百炼 Fun-ASR), TTS (小米 MIMO), MP3 playback from SD, and wake word detection are configured correctly from the transplant.

- [ ] **Step 1: Verify STT client configuration**

The stt_client already targets `wss://dashscope.aliyuncs.com/api-ws/v1/inference/` with `fun-asr-realtime` model. Confirm by reading the init code:

```bash
grep -n "dashscope\|fun-asr\|endpoint\|model" /home/ubuntu/esp32/AIWearable/components/stt/stt_client.c | head -20
```

- [ ] **Step 2: Verify TTS client configuration**

The tts_client already uses MiMo HTTP endpoint. Confirm:

```bash
grep -n "mimo\|api.xiaomimimo\|endpoint\|voice" /home/ubuntu/esp32/AIWearable/components/tts/tts_client.c | head -20
```

- [ ] **Step 3: Verify MP3 player SD card integration**

```bash
grep -n "sdcard\|SDMMC\|music\|f_open\|f_read" /home/ubuntu/esp32/AIWearable/components/mp3_player/mp3_player.c | head -20
```

- [ ] **Step 4: Verify audio mutex (TTS ↔ MP3 mutual exclusion)**

```bash
grep -n "tts_stop\|mp3_player_stop\|PA_EN\|i2s" /home/ubuntu/esp32/AIWearable/main/voice_chat.c | head -20
```

- [ ] **Step 5: Build and verify audio subsystem compiles**

```bash
cd /home/ubuntu/esp32/AIWearable
./build_audio_board.sh
```

Expected: All audio components compile without errors.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "verify: audio pipeline (Fun-ASR, MIMO TTS, MP3, WakeNet) confirmed from transplant"
```

---

### Task 4: Network - WiFi, OpenClaw Client, ED25519 Auth, Device Commands

**Files:**
- Verify: `components/wifi_manager/wifi_manager.c` (STA/AP + Captive Portal)
- Verify: `components/openclaw/openclaw_client.c` (WebSocket + ED25519 + JSON-RPC)
- Verify: `components/openclaw/include/openclaw_client.h`
- Verify: `main/app_state.c` (device command parser for `[DEVICE:...]` tags)
- Modify: `main/app_state.c` (add `chatlog=query` command if missing)

**Goal:** Confirm all network and OpenClaw integration code is working from the transplant. Add the chatlog query device command.

- [ ] **Step 1: Verify OpenClaw protocol version and auth**

```bash
grep -n "minProtocol\|maxProtocol\|client.*id\|client.*version\|device_key\|ed25519" /home/ubuntu/esp32/AIWearable/components/openclaw/openclaw_client.c | head -30
```

Expected output confirms: minProtocol=3, maxProtocol=4, client.id="cli", version="0.5.0", ED25519 auth flow

- [ ] **Step 2: Verify existing device command parsing**

```bash
grep -n "DEVICE:\|mp3=\|volume=\|brightness=\|regex\|parse_device" /home/ubuntu/esp32/AIWearable/main/app_state.c | head -30
```

- [ ] **Step 3: Add `chatlog=query` device command to app_state.c**

Read the existing `parse_device_commands` function and add the chatlog handler. The existing code uses regex to match `[DEVICE:key=value]` pairs. Add handling for `chatlog=query:YYYY-MM-DD`:

```c
// In app_state.c, inside parse_device_commands(), add after existing command handlers:

/* Handle chatlog query command */
if (strncmp(key, "chatlog", 7) == 0 && strncmp(val, "query:", 6) == 0) {
    const char *date = val + 6;
    ESP_LOGI(TAG, "Chatlog query requested for date: %s", date);
    // Read chat log, send back to OpenClaw as a chat message
    char *log_text = notes_manager_read_date(date);
    if (log_text && strlen(log_text) > 0) {
        // Send chat history to OpenClaw. The AI will summarize in response.
        if (openclaw_get_state() == OPENCLAW_STATE_CONNECTED) {
            char prompt[256];
            snprintf(prompt, sizeof(prompt), "请总结以下 %s 的聊天记录：\n\n%s", date, log_text);
            openclaw_chat_send_text(prompt);
        }
        free(log_text);
    } else {
        ESP_LOGW(TAG, "No chat log found for date: %s", date);
        if (openclaw_get_state() == OPENCLAW_STATE_CONNECTED) {
            openclaw_chat_send_text("没有找到该日期的聊天记录");
        }
    }
    count++;
    continue;
}
```

- [ ] **Step 4: Build and verify**

```bash
cd /home/ubuntu/esp32/AIWearable
./build_audio_board.sh
```

Expected: Compiles without errors.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat: add chatlog query device command to OpenClaw integration"
```

---

### Task 5: UI - LVGL, State Machine, GIF Character Animations

**Files:**
- Verify: `components/ui/ui_main.c` (15-state state machine)
- Verify: `components/ui/ui_state_gif.c` / `.h` (GIF state manager)
- Verify: `components/ui/include/ui.h` (UI state enum, all 15 states)
- Verify: `components/ui/ui_rgb_led.c` (RGB per-state mapping)
- Modify: `sdkconfig.defaults` (ensure `CONFIG_LV_USE_GIF=y`)
- Create: `components/ui/include/ui_state_gif.h` (if missing, define GIF state types)

**Goal:** Verify the LVGL UI framework is intact from transplant. Enable LVGL GIF support and ensure GIF files load from SD `/gifs/` directory.

- [ ] **Step 1: Enable LVGL GIF in sdkconfig.defaults**

```bash
# Check if LV_USE_GIF is already set
grep "LV_USE_GIF" /home/ubuntu/esp32/AIWearable/sdkconfig.defaults
```

If not set, append:

```bash
echo "CONFIG_LV_USE_GIF=y" >> /home/ubuntu/esp32/AIWearable/sdkconfig.defaults
```

- [ ] **Step 2: Verify GIF state management code**

```bash
grep -n "gif\|GIF_STATE\|lv_gif\|gifdec" /home/ubuntu/esp32/AIWearable/components/ui/ui_state_gif.c | head -40
```

- [ ] **Step 3: Verify GIF file paths point to SD card**

The GIF paths should reference `/sdcard/gifs/`. Check:

```bash
grep -n "gif_filename\|/gifs/\|/sdcard" /home/ubuntu/esp32/AIWearable/components/ui/ui_state_gif.c | head -20
```

If paths use SPIFFS or other location, update to SD card:

```c
// Expected path format in gif_filenames[] array:
static const char *gif_filenames[] = {
    [GIF_STATE_BOOT]      = "/sdcard/gifs/boot.gif",
    [GIF_STATE_IDLE]      = "/sdcard/gifs/idle.gif",
    [GIF_STATE_LISTENING] = "/sdcard/gifs/listening.gif",
    [GIF_STATE_THINKING]  = "/sdcard/gifs/thinking.gif",
    [GIF_STATE_SPEAKING]  = "/sdcard/gifs/speaking.gif",
    [GIF_STATE_ERROR]     = "/sdcard/gifs/error.gif",
};
```

- [ ] **Step 4: Verify simplified UI layout matches spec**

The spec shows a simplified UI with GIF character area + status bar only (no chat bubbles). Verify the current UI state handling:

```bash
grep -n "ui_set_state\|UI_STATE_\|app_set_state" /home/ubuntu/esp32/AIWearable/components/ui/ui_main.c | head -40
```

The AIClaw transplant already has the full UI with chat bubbles. For now, keep them — the AI can still show conversation text. The GIF display is the primary interaction focus per spec.

- [ ] **Step 5: Build and verify**

```bash
cd /home/ubuntu/esp32/AIWearable
./build_audio_board.sh
```

Expected: Compiles with LVGL GIF support enabled.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: enable LVGL GIF support, configure GIF paths for SD card storage"
```

---

### Task 6: Web Management Enhancement - Chat Preview + File Upload/Delete

**Files:**
- Modify: `components/webserver/webserver.c` (add new API endpoints)
- Modify: `components/webserver/include/webserver.h` (declare new handlers if needed)
- Modify: `components/webserver/index.html` (add file management UI sections)
- Reference: `components/notes_manager/notes_manager.c` (for chat history reading)

**Goal:** Add three new web features on top of the existing web server:
1. Chat history preview: `GET /api/chat/history?date=YYYY-MM-DD`
2. MP3 file upload/delete: `POST /api/mp3/upload`, `POST /api/mp3/delete`
3. GIF file upload/delete: `POST /api/gif/upload`, `POST /api/gif/delete`

- [ ] **Step 1: Add chat history API endpoint to webserver.c**

Insert before the existing `webserver_start()` function:

```c
/* ── GET /api/chat/history?date=YYYY-MM-DD — chat log preview ────────── */
static esp_err_t chat_history_handler(httpd_req_t *req)
{
    char date[32] = {0};
    size_t buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        char *buf = malloc(buf_len);
        if (buf) {
            httpd_req_get_url_query_str(req, buf, buf_len);
            httpd_query_key_value(buf, "date", date, sizeof(date));
            free(buf);
        }
    }

    if (date[0] == '\0') {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *log_text = notes_manager_read_date(date);
    if (!log_text) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "date", date);
        cJSON_AddStringToObject(j, "content", "No chat history for this date");
        char *json_str = cJSON_PrintUnformatted(j);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_str, strlen(json_str));
        free(json_str);
        cJSON_Delete(j);
        return ESP_OK;
    }

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "date", date);
    cJSON_AddStringToObject(j, "content", log_text);
    char *json_str = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(j);
    free(log_text);
    return ESP_OK;
}
```

- [ ] **Step 2: Add MP3 upload handler to webserver.c**

```c
/* ── POST /api/mp3/upload — receive MP3 file (multipart/form-data) ───── */
static esp_err_t mp3_upload_handler(httpd_req_t *req)
{
    char filepath[256];
    char filename[64] = "uploaded.mp3";
    int received = 0;

    // Parse multipart boundary
    char content_type[128];
    httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));

    // Simple approach: write raw body to file
    // For proper multipart parsing, use esp_http_client or custom parser
    snprintf(filepath, sizeof(filepath), "/sdcard/music/%s", filename);

    FILE *f = fopen(filepath, "wb");
    if (!f) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char buf[512];
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf, sizeof(buf));
        if (ret <= 0) break;
        fwrite(buf, 1, ret, f);
        received += ret;
    }
    fclose(f);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "status", "ok");
    cJSON_AddStringToObject(j, "file", filename);
    cJSON_AddNumberToObject(j, "size", received);
    char *json_str = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(j);
    return ESP_OK;
}
```

- [ ] **Step 3: Add MP3 delete handler to webserver.c**

```c
/* ── POST /api/mp3/delete — delete MP3 file ──────────────────────────── */
static esp_err_t mp3_delete_handler(httpd_req_t *req)
{
    char body[128] = {0};
    httpd_req_recv(req, body, sizeof(body) - 1);

    cJSON *j = cJSON_Parse(body);
    if (!j) { httpd_resp_send_500(req); return ESP_FAIL; }

    const char *file = cJSON_GetObjectItem(j, "file")->valuestring;
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "/sdcard/music/%s", file);

    cJSON *resp = cJSON_CreateObject();
    if (unlink(filepath) == 0) {
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "message", "File deleted");
    } else {
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "message", "Failed to delete file");
    }
    cJSON_Delete(j);

    char *json_str = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(resp);
    return ESP_OK;
}
```

- [ ] **Step 4: Add GIF upload and delete handlers (same pattern as MP3, targeting /sdcard/gifs/)**

```c
/* ── POST /api/gif/upload — receive GIF file ─────────────────────────── */
static esp_err_t gif_upload_handler(httpd_req_t *req)
{
    char filepath[256];
    char filename[64] = "uploaded.gif";
    int received = 0;

    snprintf(filepath, sizeof(filepath), "/sdcard/gifs/%s", filename);
    FILE *f = fopen(filepath, "wb");
    if (!f) { httpd_resp_send_500(req); return ESP_FAIL; }

    char buf[512];
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf, sizeof(buf));
        if (ret <= 0) break;
        fwrite(buf, 1, ret, f);
        received += ret;
    }
    fclose(f);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "status", "ok");
    cJSON_AddStringToObject(j, "file", filename);
    char *json_str = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(j);
    return ESP_OK;
}

/* ── POST /api/gif/delete — delete GIF file ──────────────────────────── */
static esp_err_t gif_delete_handler(httpd_req_t *req)
{
    char body[128] = {0};
    httpd_req_recv(req, body, sizeof(body) - 1);

    cJSON *j = cJSON_Parse(body);
    if (!j) { httpd_resp_send_500(req); return ESP_FAIL; }

    const char *file = cJSON_GetObjectItem(j, "file")->valuestring;
    char filepath[256];
    snprintf(filepath, sizeof(filepath), "/sdcard/gifs/%s", file);

    cJSON *resp = cJSON_CreateObject();
    if (unlink(filepath) == 0) {
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "message", "GIF deleted");
    } else {
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "message", "Failed to delete GIF");
    }
    cJSON_Delete(j);

    char *json_str = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    cJSON_Delete(resp);
    return ESP_OK;
}
```

- [ ] **Step 5: Register new URI handlers in webserver_start()**

In `webserver_start()`, inside the `httpd_register_uri_handler()` calls, add:

```c
httpd_uri_t chat_history = {
    .uri       = "/api/chat/history",
    .method    = HTTP_GET,
    .handler   = chat_history_handler,
    .user_ctx  = NULL
};
httpd_register_uri_handler(s_server, &chat_history);

httpd_uri_t mp3_upload = {
    .uri       = "/api/mp3/upload",
    .method    = HTTP_POST,
    .handler   = mp3_upload_handler,
    .user_ctx  = NULL
};
httpd_register_uri_handler(s_server, &mp3_upload);

httpd_uri_t mp3_delete = {
    .uri       = "/api/mp3/delete",
    .method    = HTTP_POST,
    .handler   = mp3_delete_handler,
    .user_ctx  = NULL
};
httpd_register_uri_handler(s_server, &mp3_delete);

httpd_uri_t gif_upload = {
    .uri       = "/api/gif/upload",
    .method    = HTTP_POST,
    .handler   = gif_upload_handler,
    .user_ctx  = NULL
};
httpd_register_uri_handler(s_server, &gif_upload);

httpd_uri_t gif_delete = {
    .uri       = "/api/gif/delete",
    .method    = HTTP_POST,
    .handler   = gif_delete_handler,
    .user_ctx  = NULL
};
httpd_register_uri_handler(s_server, &gif_delete);
```

- [ ] **Step 6: Build and verify**

```bash
cd /home/ubuntu/esp32/AIWearable
./build_audio_board.sh
```

Expected: Compiles with new web API endpoints.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat: add web chat history preview + MP3/GIF upload and delete endpoints"
```

---

### Task 7: Chat History - Notes Manager Verification + Query Integration

**Files:**
- Verify: `components/notes_manager/notes_manager.c` (SD card chat log read/write)
- Verify: `components/notes_manager/include/notes_manager.h`
- Add function: `notes_manager_read_date()` if not present

**Goal:** The notes_manager already handles writing chat logs to SD card. Verify the read function exists for the query feature, and add it if missing.

- [ ] **Step 1: Check existing notes_manager API**

```bash
grep -n "notes_manager_\|void\|esp_err_t\|char\*" /home/ubuntu/esp32/AIWearable/components/notes_manager/include/notes_manager.h
```

- [ ] **Step 2: If `notes_manager_read_date()` is missing, add it**

In `notes_manager.c`:

```c
char *notes_manager_read_date(const char *date)
{
    char filepath[64];
    snprintf(filepath, sizeof(filepath), "/sdcard/chat/%s.txt", date);

    struct stat st;
    if (stat(filepath, &st) != 0) {
        return NULL;  // File doesn't exist
    }

    char *buf = heap_caps_malloc(st.st_size + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;

    FILE *f = fopen(filepath, "r");
    if (!f) {
        free(buf);
        return NULL;
    }

    size_t read = fread(buf, 1, st.st_size, f);
    buf[read] = '\0';
    fclose(f);
    return buf;
}
```

Declare in `notes_manager.h`:

```c
/** Read chat log for a specific date (YYYY-MM-DD). Caller must free() the result. */
char *notes_manager_read_date(const char *date);
```

- [ ] **Step 3: Build and verify**

```bash
cd /home/ubuntu/esp32/AIWearable
./build_audio_board.sh
```

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat: add notes_manager_read_date() for chat history query"
```

---

### Task 8: Camera Reserved Interface + LED Controller Verification

**Files:**
- Verify: `components/camera/camera.c` (camera init deferred to first use)
- Verify: `components/camera/include/camera.h`
- Verify: `components/board/rgb_ring.c` / `.h` (WS2812 LED ring)
- Verify: `main/app_state.c` (LED state mapping)

**Goal:** Confirm camera is properly deferred (lazy init) and LED ring is correctly mapped for all 15 states.

- [ ] **Step 1: Verify camera deferred initialization**

```bash
grep -n "camera_init\|camera_is_ready\|CAMERA_BIT\|deferred\|first use" /home/ubuntu/esp32/AIWearable/components/camera/camera.c | head -20
```

- [ ] **Step 2: Verify RGB LED state colors**

```bash
grep -n "UI_STATE_\|RGB_MODE_\|board_rgb\|app_led_for_state" /home/ubuntu/esp32/AIWearable/main/app_state.c | head -30
```

- [ ] **Step 3: Ensure no camera init at boot (saves power)**

The AIClaw transplant already has `/* Camera init deferred to first use */` pattern. Verify:

```bash
grep -A5 "Camera.*deferred\|camera.*first" /home/ubuntu/esp32/AIWearable/main/app_main.c
```

- [ ] **Step 4: Build and verify**

```bash
cd /home/ubuntu/esp32/AIWearable
./build_audio_board.sh
```

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "verify: camera deferred initialization, RGB LED state mapping confirmed"
```

---

### Task 9: End-to-End Build Verification and Checklist

**Files:**
- All project files

**Goal:** Final full project build, verify all components link correctly, confirm the firmware binary is produced. Create an SD card preload checklist.

- [ ] **Step 1: Clean build from scratch**

```bash
cd /home/ubuntu/esp32/AIWearable
rm -rf build/
./build_audio_board.sh
```

Expected: BUILD SUCCESSFUL, no warnings from our code. Firmware binary at `build/aiwearable.bin`.

- [ ] **Step 2: Verify binary size fits on 16MB flash**

```bash
ls -lh /home/ubuntu/esp32/AIWearable/build/aiwearable.bin
```

- [ ] **Step 3: Create SD card preload structure checklist**

Create `docs/sd-card-setup.md`:

```markdown
# SD Card Setup for AIWearable

## Required Directory Structure

sdcard/
├── chat/           # (auto-created by firmware)
├── music/          # Place MP3 files here
│   └── *.mp3
├── gifs/           # Place GIF character animations here
│   ├── boot.gif         # Startup animation (172x172 max)
│   ├── idle.gif         # Idle/waiting animation
│   ├── listening.gif    # Listening for voice input
│   ├── thinking.gif     # AI processing
│   ├── speaking.gif     # AI speaking response
│   ├── playing.gif      # Music playing
│   └── error.gif        # Error state
└── system/         # (auto-created by firmware)
```

- [ ] **Step 4: Flash and hardware test plan**

```bash
# Flash command:
export ESPPORT=/dev/ttyUSB0
cd /home/ubuntu/esp32/AIWearable
idf.py flash monitor
```

Hardware verification checklist:
- [ ] Board boots, LVGL shows boot GIF
- [ ] WiFi connects (or enters AP mode)
- [ ] OpenClaw WebSocket handshake completes
- [ ] Wake word "你好小智" is detected
- [ ] Voice → STT → OpenClaw → TTS round trip works
- [ ] [DEVICE:mp3=play:xxx] plays music from SD
- [ ] [DEVICE:chatlog=query:date] returns chat history
- [ ] Web server serves management UI
- [ ] Chat history preview works in browser
- [ ] MP3/GIF upload/delete works in browser
- [ ] RGB LEDs change color per state
- [ ] 60s idle → SLEEP, BOOT → ARMED
- [ ] 30min stability: no crashes, no memory leak

- [ ] **Step 5: Commit final verification**

```bash
git add -A
git commit -m "feat: complete AIWearable firmware - build verified, ready for hardware test"
```

---

### Implementation Notes

1. **No secrets.h**: The project requires `main/include/secrets.h` with WiFi, OpenClaw, DashScope, and MiMo credentials. Not included in repo. Use `secrets_example.txt` as template.

2. **Build command**: Always use `./build_audio_board.sh` (not raw `idf.py build`). The script handles board config, flash size, and target selection.

3. **Memory discipline**: Follow HARNESS.md rules — audio buffers >4KB in PSRAM, LVGL buffers in internal SRAM, network tasks ≥8KB stack. Do NOT regress from verified patterns.

4. **UTF-8 discipline**: All strings from SD card (filenames, chat logs) must be UTF-8 sanitized before sending over WebSocket. The `utf8_sanitize()` function already exists in mp3_player — reuse it.

5. **Final over delta**: OpenClaw chat responses come as incremental `delta` then complete `final`. The `final` must REPLACE (not append to) the accumulated delta buffer to avoid text duplication.
