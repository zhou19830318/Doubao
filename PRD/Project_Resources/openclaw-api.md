# OpenClaw Gateway WebSocket API Reference

## Connection Flow
1. Client connects to `ws://HOST:18789`
2. Server sends `{type:"event", event:"connect.challenge", payload:{nonce:"..."}}`
3. Client sends `{type:"req", id:"1", method:"connect", params:{...}}` with ED25519 signed auth
4. Server responds with `hello-ok` containing snapshot data (version, uptime, health, agents, sessions)
5. Connection is now authenticated with `operator.read` + `operator.write` scopes

## Available API Methods

### `health` (GET health status)
**Request**: `{type:"req", id:"N", method:"health", params:{}}`
**Response fields**:
- `ok`: boolean — overall health
- `agents`: array — `{agentId, name, isDefault, heartbeat, sessions: {count, recent: [{key, age}]}}`
- `channels`: object — per-channel health e.g. `{whatsapp: {connected, linked, configured, probe}}`
- `channelOrder`, `channelLabels`: for UI display
- `sessions`: `{path, count, recent}`
- `heartbeatSeconds`: default heartbeat

### `usage.status` (GET usage/cost data)
**Request**: `{type:"req", id:"N", method:"usage.status", params:{}}`
**Response fields**:
- `updatedAt`: timestamp
- `providers`: array — may be EMPTY if no usage tracking configured
  - `{provider, displayName, windows: [{label, usedPercent, resetAt?}], plan?, error?}`
- Provider IDs: "anthropic", "openai-codex", "google-gemini-cli", "github-copilot", etc.
- **Empty providers**: Normal when no API keys configured for usage tracking

### `chat.send` (Send chat message)
**Request**: `{type:"req", id:"N", method:"chat.send", params:{sessionKey:"...", message:{content:[{type:"text", text:"..."}]}, idempotencyKey:"..."}}`
**Response**: `{type:"res", payload:{runId, status:"started"}}`
**Events**: Server pushes `{type:"event", event:"chat", payload:{state:"delta"|"final", message:{content:[{text}]}}}`

### `channels.status` (GET channel config)
Returns detailed channel configuration with probe capability.

### `sessions.list` (GET all sessions)
Returns session list with token counts, models, last messages.

## Server Push Events
- `connect.challenge` — auth challenge with nonce
- `chat` — streaming chat response (delta/final states)
- `tick` — heartbeat every ~1s (increment uptime)
- `health` — periodic health push (~30s)

## Key Learnings
1. **Binary audio NOT supported** — All WebSocket messages are JSON text. Use STT locally, send text.
2. **Usage may be empty** — Check `providers.length > 0` before accessing data
3. **Health response has `channels.whatsapp`** — check `connected`, `linked`, `configured` booleans
4. **Auth ping fix** — Set `ping_interval_sec=1` to avoid 10s default timeout during auth handshake
5. **Response IDs are strings** — Match request IDs to responses for proper routing
