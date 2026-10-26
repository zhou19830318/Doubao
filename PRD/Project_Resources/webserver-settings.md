# Web Server & Settings Architecture

## Settings Component (`components/settings/`)

### Storage
- NVS namespace: `"heyclawy"`
- Each field stored as individual NVS key (max 15 chars)
- Key mapping: `wifi_ssid` → "wifi_ssid", `oc_host` → "oc_host", `volume` → "volume", etc.

### API
```c
esp_err_t settings_init(const settings_t *defaults);  // Call once at boot
const settings_t *settings_get(void);                  // Thread-safe read
settings_t *settings_get_mutable(void);                // Get writable pointer
esp_err_t settings_save(void);                         // Persist to NVS
esp_err_t settings_to_json(char *buf, size_t len, bool show_secrets);
esp_err_t settings_from_json(const char *json);        // Skips "****" fields
esp_err_t settings_reset_defaults(void);
```

### Important Design Decisions
1. **No circular dependency**: Settings component cannot depend on `main/`. The `settings_init()` function accepts a `settings_t *defaults` struct populated by `app_main.c` from `secrets.h` constants.
2. **Secret masking**: `settings_to_json()` replaces sensitive fields (passwords, tokens, keys) with `"****"` unless `show_secrets=true`. `settings_from_json()` skips any field containing `"****"`.
3. **Immediate apply**: When volume or brightness changes via web API, `board_audio_set_volume()` and `board_display_set_brightness()` are called immediately without reboot.

## Web Server Component (`components/webserver/`)

### Endpoints
| Method | Path | Description |
|--------|------|-------------|
| GET | `/` | Embedded SPA HTML page |
| GET | `/api/status` | Device + OpenClaw status JSON |
| GET | `/api/settings` | All settings (secrets masked) |
| PUT | `/api/settings` | Update settings (partial JSON OK) |
| POST | `/api/settings/reset` | Reset all to defaults |
| GET | `/api/errors` | Error log (ring buffer, newest first) |
| POST | `/api/openclaw/test` | Test OC connection params |
| POST | `/api/reboot` | Restart device |
| OPTIONS | `/api/*` | CORS preflight |

### HTML Embedding
- `index.html` is embedded via `EMBED_FILES` in `CMakeLists.txt`
- Accessible as `_binary_index_html_start` / `_binary_index_html_end`
- Size: ~19KB (single SPA with all CSS/JS inline)

### Configuration
- Stack size: 8192 bytes
- Max URI handlers: 16
- Max open sockets: 7
- CORS enabled for all `/api/*` routes

## Error Log Component (`components/error_log/`)
- Ring buffer of 32 entries
- Each entry: `int64_t timestamp`, `error_log_source_t` (DEVICE/OPENCLAW/WIFI/STT/TTS), `error_log_severity_t` (INFO/WARN/ERROR/CRITICAL), `char message[128]`
- `error_log_add()` also calls `ESP_LOGW` for serial visibility
- JSON export for web UI consumption

## Web Server Toggle
- Event: `WEBSERVER_TOGGLE_BIT (BIT4)` in `g_app_events`
- Serial command: `web` or `w`
- On start: shows device IP on screen for 5 seconds
- Status bar: globe icon turns blue when running
- Default: OFF (can be changed in settings)

## Testing the Web Server
```powershell
# Start via serial
# In idf_monitor, type: web

# Test API
Invoke-WebRequest -Uri "http://<device-ip>/api/status"
Invoke-WebRequest -Uri "http://<device-ip>/api/settings"

# Save settings
$body = '{"volume":80}'
Invoke-WebRequest -Uri "http://<device-ip>/api/settings" -Method PUT -Body $body -ContentType "application/json"
```
