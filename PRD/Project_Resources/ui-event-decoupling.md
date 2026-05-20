# Skill: UI-to-Main Event Decoupling

## Problem
The `components/ui/` module cannot `#include` files from `main/include/` (e.g., `app_state.h`) because components cannot depend on the main module — this creates a circular dependency.

## Solution: Setter Pattern
1. **UI side** (`ui.c`):
   - Static `void *s_events` variable
   - `ui_set_event_group(void *eg)` function sets `s_events`
   - Local constants duplicate event bit values:
     ```c
     #define UI_TTS_PLAY_BIT        (1 << 3)   // BIT3 — must match app_state.h
     #define UI_DETAILS_BIT         (1 << 5)   // BIT5
     #define UI_WEBSERVER_TOGGLE_BIT (1 << 4)  // BIT4
     ```
   - Button callbacks cast `s_events` to `EventGroupHandle_t` and call `xEventGroupSetBits()`

2. **Main side** (`app_main.c`):
   - After creating `g_app_events` event group, calls:
     ```c
     ui_set_event_group(g_app_events);
     ```

## ⚠️ CRITICAL
- The event bit values in `ui.c` **MUST** stay in sync with those in `main/include/app_state.h`
- If you add/change event bits in `app_state.h`, update the corresponding defines in `ui.c`
- Current bits: KNOB=BIT2, TTS_PLAY=BIT3, WEBSERVER_TOGGLE=BIT4, DETAILS=BIT5, CANCEL=BIT6

## Files
- `components/ui/ui.c` — UI_*_BIT defines + s_events + setter + callbacks
- `components/ui/include/ui.h` — `ui_set_event_group()` declaration
- `main/include/app_state.h` — Authoritative event bit definitions
- `main/app_main.c` — Calls `ui_set_event_group()` during init
