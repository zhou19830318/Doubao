# WebSocket Auth Delay Fix (ping_interval_sec)

## Problem
After ESP32 connects to OpenClaw WebSocket gateway, the `connect.challenge` message takes exactly 10 seconds to be received. The server's `DEFAULT_HANDSHAKE_TIMEOUT_MS = 10,000` closes the connection, causing auth failure with close code 1000.

## Root Cause
The ESP WebSocket client library (`espressif__esp_websocket_client`) has a buffering issue:

1. After `esp_transport_connect()` succeeds, the library fires CONNECTED event
2. The server sends `connect.challenge` immediately during WS upgrade
3. First loop iteration: `read_select` starts at 0 → library **SKIPS reading** (line 898-900)
4. Then `esp_transport_poll_read(transport, 1000ms)` polls the raw socket
5. Challenge data is buffered in the transport layer during WS upgrade — invisible to socket-level poll
6. `poll_read` returns 0 repeatedly because no **new** socket data arrives
7. The ONLY thing that triggers new socket data is a PING/PONG exchange
8. Default `WEBSOCKET_PING_INTERVAL_SEC = 10` (set when config value is 0)
9. First PING fires after ~10s of uptime → PONG response flushes buffered data
10. By then, server has already closed the connection (10s handshake timeout)

## Fix
Set `ping_interval_sec = 1` in the WebSocket client config:

```c
esp_websocket_client_config_t ws_cfg = {
    .uri = "ws://<OC_HOST>:<OC_PORT>",
    .ping_interval_sec = 1,  // ← CRITICAL: triggers PING within ~1s, flushing buffered challenge
    // ... other config
};
```

## Result
- Before: Challenge received after 10,022ms → auth timeout
- After: Challenge received after 14ms → auth complete in ~150ms total

## Key Files
- `managed_components/espressif__esp_websocket_client/esp_websocket_client.c`:
  - Line 35: `WEBSOCKET_PING_INTERVAL_SEC = 10` (default)
  - Line 387-388: Falls back to default when config is 0
  - Line 898-900: Skips reading when `read_select == 0`
  - Line 936: `esp_transport_poll_read(transport, 1000)` poll timeout

- `components/openclaw/openclaw_client.c`:
  - WebSocket config with `ping_interval_sec = 1`

## OpenClaw Server Constants
- `DEFAULT_HANDSHAKE_TIMEOUT_MS = 10_000` (openclaw/src/gateway/server-constants.ts:23)
- `DEVICE_SIGNATURE_SKEW_MS = 10 * 60 * 1000` (10 min) — signature expiry is NOT the issue
- Close code 1000 = handshake timeout
- Close code 1008 = auth failure (nonce mismatch, etc.)

## Reconnect Fix (gateway restart)

After auth completes, ping is relaxed to 30s for power saving:
```c
// After auth complete:
esp_websocket_client_set_ping_interval_sec(s_oc.ws, 30);
```

**BUG**: When the gateway restarts and the device auto-reconnects, `WEBSOCKET_EVENT_CONNECTED` fires but ping is still at 30s from the previous session. The new challenge data can't be flushed within 10s → auth fails every time.

**FIX**: Reset ping to 1s in `WEBSOCKET_EVENT_CONNECTED` handler:
```c
case WEBSOCKET_EVENT_CONNECTED:
    esp_websocket_client_set_ping_interval_sec(s_oc.ws, 1);  // reset for new auth
    set_state(OPENCLAW_STATE_CONNECTING);
    break;
```

The ESP WS client's `reconnect_timeout_ms = 5000` handles automatic reconnection after disconnect.
