# OpenClaw Hooks API & External Activity Tracking

## OpenClaw Hooks API

### Overview
OpenClaw exposes HTTP POST endpoints (`/hooks/*`) for external integrations. These allow triggering agent turns without WebSocket connections.

### Configuration
Hooks must be explicitly enabled in `~/.openclaw/openclaw.json`:
```json
{
  "hooks": {
    "enabled": true,
    "token": "<HOOKS_TOKEN>",
    "internal": {
      "enabled": true,
      "entries": { "session-memory": { "enabled": true } }
    }
  }
}
```

**IMPORTANT**: `hooks.token` MUST be different from `gateway.auth.token`. OpenClaw rejects identical tokens.

### Endpoints

#### Send Agent Message
```
POST http://<oc-host>:18789/hooks/agent
Authorization: Bearer <hooks_token>
Content-Type: application/json

{
  "message": "Your message here",
  "name": "HeyClawy-Test"  // optional, identifies caller
}

Response: 202 {"ok": true, "runId": "uuid"}
```

#### Wake Agent
```
POST http://<oc-host>:18789/hooks/wake
Authorization: Bearer <hooks_token>
```

### Error Responses
- `400` — Invalid payload
- `401` — Unauthorized (wrong token)
- `405` — Method not allowed (hooks not enabled, or GET instead of POST)
- `413` — Payload too large (default max: 256KB)

### Restarting OpenClaw After Config Change
```bash
# Find PID
ps aux | grep openclaw

# Kill and restart
kill <pid>
nohup /home/user/.npm-global/bin/openclaw gateway --port 18789 > /tmp/oc.log 2>&1 &
```
Note: Non-interactive SSH doesn't have npm-global in PATH — use full path to binary.

## External Activity Tracking on Device (MVP12 Redesign)

### Architecture: Event-Based Activity Detection
Device handles ALL WebSocket event types for real-time activity tracking:

#### 1. Agent Events (`type: "agent"`)
- **`stream: "lifecycle"`** — Agent run start/end phases
  - `phase: "start"` → Set `is_active=true`, store `runId`, `sessionKey`
  - `phase: "end"` → Clear active state (if matching runId)
- **`stream: "tool"`** — Tool invocation during agent run
  - `active_detail` set to tool name (e.g., "Tool: web_search")
- **`stream: "assistant"`** — LLM is generating response text
  - `active_detail` set to "Responding..."

#### 2. Chat Events (`type: "chat"`)
- **`state: "delta"`** — Streaming text chunks
- **`state: "final"`** — Complete response
- **`state: "error"`** — Error occurred
- **`state: "aborted"`** — User or system cancelled (NEWLY HANDLED in MVP12)
- External detection: When `chat_cb` is NULL, incoming events are external

#### 3. Cron Events (`type: "cron"`)
- **`action: "start"`** — Cron job begins, shows job name from cached task list
- **`action: "end"`** — Cron job completed
- **`action: "error"`** — Cron job failed

#### 4. Health Events (Enhanced)
- Now parses: `uptimeMs`, `sessions.count`, `sessions.recent[0].age`, `channels.whatsapp`
- **Fallback detection**: If `last_activity_sec < 5` from health and not already tracking → mark active

### Activity State Fields (`openclaw_info_t`)
```c
bool is_active;              // Real-time: something is running right now
bool is_external;            // Activity came from non-device source
char active_source[24];      // "chat", "agent", "cron", "health"
char active_run_id[48];      // For abort support
char active_session_key[48]; // For abort support
char active_detail[64];      // "Exec...", "Memory", "Responding", etc.
int64_t active_started_ms;   // Epoch ms when activity started
int active_count;            // Concurrent activities count

// Multi-run carousel (MVP16)
openclaw_active_run_t active_runs[OC_MAX_ACTIVE_RUNS]; // Up to 8 concurrent runs
int active_runs_count;       // Number of active run slots
int carousel_index;          // Rotates every 3s in UI
```

### Multi-Run Carousel (MVP16)
- `add_run_slot()`: agent lifecycle.start → allocates slot, evicts oldest if full
- `find_run_slot()`: lookup by runId
- `remove_run_slot()`: lifecycle.end → frees slot; if count reaches 0, calls `clear_activity()`
- UI shows `[n/N]` indicator + per-run elapsed timer
- Carousel index rotated every 6 status ticks (3s) in `status_update_task`
- Setting: `activity_carousel` (default: on)

### Notification Detection (MVP16)
- Device tracks its own runIds in `device_run_ids[8]` ring buffer
- `track_device_run()`: called on first chat delta from device-initiated request
- `is_device_run()`: checks if a runId was device-initiated
- When external `chat.final` arrives: extract text → call `notify_cb(text, source)`
- `openclaw_set_notify_cb()`: register notification handler
- In app_main: `on_openclaw_notify()` → auto-TTS + display on UI_STATE_RESPONSE + amber LED
- `g_tts_text[1024]`: shared buffer for notification text (TTS task checks before `ui_get_full_response()`)
- Setting: `auto_notify` (default: on)

### Clearing Activity (Cooldown Mechanism)
```c
// clear_activity() helper — call from ALL activity-end paths
static void clear_activity(void) {
    s_oc.info.is_active = false;
    s_oc.info.is_external = false;
    s_oc.info.active_detail[0] = '\0';
    // ... clear all tracking fields
    s_oc.info.last_activity_sec = 30;
    // Set cooldown timestamp
    s_oc.active_cleared_ms = now_ms;  // Prevents health fallback for 10s
}
```
**Key**: Health fallback checks `active_cleared_ms` — if cleared < 10s ago, suppresses re-activation.

### Tool Event Registration
- Device must send `caps: ["tool-events"]` in connect request params
- Tool events (`stream: "tool"`) are **per-run/per-initiator** — only sent to the client that started the run
- External runs: device only gets lifecycle + assistant events (no tool details)
- Device-initiated runs: device gets ALL events including tool names
- `format_tool_label()` converts "memory_search" → "Memory", "exec" → "Exec"

### Abort Support
```c
// Wheel spin during IDLE + external activity → abort
openclaw_chat_abort_session(info->active_session_key);
// Sends: {"method": "chat.abort", "params": {"sessionKey": "..."}}
```

### UI Behavior
- **ACTIVE state**: Big label shows detail text (blue) — "Thinking...", "Exec...", "Memory", "Responding"
- **READY state**: Big label "READY" (green), "Tap or Wheel"
- **IDLE restore**: Any non-"READY" text → restore when `is_active` becomes false (not just "ACTIVE" label)
- **Response dismiss**: Touch or wheel spin during UI_STATE_RESPONSE → dismiss to IDLE
- **Thinking detail**: `ui_set_thinking_detail()` shows tool name in orange big label + timer in sub-label
- RGB LED: active → blue breathe, bg tasks → teal breathe

### CLI Pairing Issue
⚠️ `openclaw agent` CLI command falls back to "embedded" mode (invisible to device) when not paired.
- Gateway auto-approves loopback (127.0.0.1) connections only
- LAN connections need manual approval: `openclaw devices approve <requestId>`
- Check pending: `openclaw devices list --pending`
- After approval, CLI connects through gateway → events broadcast to all clients

### Wake-from-Sleep Pattern (app_tasks.c)
```c
// Checked EVERY 500ms, even during sleep:
if (s_sleeping && openclaw_get_state() == OPENCLAW_STATE_CONNECTED) {
    if (openclaw_consume_external_activity()) {
        app_reset_activity_timer();  // wakes display + LED
    }
    // Slow health poll during sleep (every 15s)
    if (++counter >= 30) { openclaw_request_health(); }
    if (info->last_activity_sec < 15) {
        app_reset_activity_timer();
    }
}
```

## Testing Activity Tracking

### Method 1: OpenClaw CLI (preferred — tests agent events)
```bash
# SSH to OpenClaw server
openclaw agent --session-id main --message "Count from 1 to 5 slowly"
```
⚠️ Must be paired first! If it says "pairing required", approve the request.

### Method 2: Hooks API (curl — tests chat events)
```powershell
$body = '{"message":"What is 2+2?","name":"Test"}'
curl -s -X POST "http://<OC_HOST>:18789/hooks/agent" -H "Content-Type: application/json" -H "Authorization: Bearer <HOOKS_TOKEN>" -d $body
```

### Expected Device Log Sequence (Agent Events)
```
Agent run started (external) run=ffec2a9e-...
OC active (ext=1, last=0s) → ACTIVE display
Agent run ended (external, phase=end) run=ffec2a9e-...
OC idle → READY display
```
