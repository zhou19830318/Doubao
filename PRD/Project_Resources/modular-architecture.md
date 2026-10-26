# Modular Code Architecture (v0.5.0+)

## File Structure
```
main/
├── app_main.c          # Init sequence + main event loop (~240 lines)
├── app_state.c         # Shared globals, LED mapping, state transitions
├── voice_chat.c        # Recording → STT → OpenClaw chat flow
├── serial_cmd.c        # Serial console command processing
├── app_tasks.c         # Background tasks (knob, status, TTS)
├── include/
│   ├── app_config.h    # Version, compile-time defaults (fallback only)
│   ├── secrets.h       # WiFi/OpenClaw credentials (.gitignored)
│   ├── app_state.h     # Shared state declarations
│   ├── voice_chat.h    # voice_chat_start() API
│   ├── serial_cmd.h    # serial_cmd_task() API
│   └── app_tasks.h     # app_tasks_start() API
├── CMakeLists.txt
```

## Shared State Pattern
All modules share state through `app_state.h`:
```c
extern EventGroupHandle_t g_app_events;   // FreeRTOS event group
extern bool g_recording;                   // True during mic recording
extern int64_t g_response_shown_at;       // Timestamp for response timeout

// Event bits
#define KNOB_PRESSED_BIT  BIT0
#define TTS_PLAY_BIT      BIT1
#define DETAILS_BIT       BIT2
#define STATUS_REQ_BIT    BIT3
#define WEBSERVER_TOGGLE_BIT BIT4

void app_set_state(app_state_t new_state);        // UI + LED state transition
void app_on_chat_response(const char *response);  // Shared callback
```

## Module Responsibilities
1. **app_main.c**: Only init (settings, board, UI, TTS, STT, WiFi, OpenClaw) and main event loop
2. **app_state.c**: State management, LED color mapping, chat response callback
3. **voice_chat.c**: Full recording pipeline (beep → record → silence detect → STT → send)
4. **serial_cmd.c**: Parses serial input, sets event bits, handles `say`/`talk`/`play`/etc.
5. **app_tasks.c**: Creates and manages FreeRTOS tasks (knob monitor, health polling, TTS player)

## Adding New Modules
1. Create `main/new_module.c` and `main/include/new_module.h`
2. Add source to `main/CMakeLists.txt` SRCS list
3. Include `app_state.h` for shared state access
4. Define new event bits if needed (BIT5, BIT6, etc.)
5. Handle events in app_main.c main loop or create a dedicated task

## CMakeLists.txt Pattern
```cmake
idf_component_register(
    SRCS "app_main.c" "app_state.c" "voice_chat.c" "serial_cmd.c" "app_tasks.c"
    INCLUDE_DIRS "include"
    REQUIRES board ui openclaw wifi_manager tts stt settings error_log webserver
)
```
