# Skill: OpenClaw Integration

## What is OpenClaw?
OpenClaw is a personal AI assistant that runs on your own devices. It provides:
- Multi-channel messaging (WhatsApp, Telegram, Slack, Discord, etc.)
- Voice wake + talk mode
- Cron jobs and automation
- WebSocket-based Gateway control plane
- CLI tools for management

## OpenClaw Gateway WebSocket Protocol

### Connection Flow
1. Client connects to `ws://HOST:18789`
2. Server sends `connect.challenge` event with a `nonce` UUID
3. Client sends `connect` request with auth credentials (token + device identity)
4. Server responds with `hello-ok` response

### Frame Format
All frames are JSON text:
- **Request**: `{type:"req", id:NonEmptyString, method:String, params?:Object}`
  - `id` MUST be a non-empty STRING (not a number!)
  - `additionalProperties: false` — no extra fields allowed
- **Response**: `{type:"res", id:String, ok:Boolean, payload?:Object, error?:Object}`
- **Event**: `{type:"event", event:String, payload?:Object}`

### Valid Client IDs
Must be one of: `"webchat-ui"`, `"openclaw-control-ui"`, `"webchat"`, `"cli"`, `"gateway-client"`, `"openclaw-macos"`, `"openclaw-ios"`, `"openclaw-android"`, `"node-host"`, `"test"`, `"fingerprint"`, `"openclaw-probe"`

### Valid Client Modes
Must be one of: `"webchat"`, `"cli"`, `"ui"`, `"backend"`, `"node"`, `"probe"`, `"test"`

### Authentication
HeyClawy uses **ED25519 device identity** authentication:
- See `.copilot/skills/openclaw-device-auth.md` for full details
- Token-only auth (without device identity) gets all scopes cleared for non-localhost connections
- The `openclaw-control-ui` client ID has origin restrictions (won't work from ESP32)
- Device identity with `gateway-client` / `cli` mode gets full `operator.read,operator.write` scopes

### Chat API
**Sending a message:**
```json
{"type":"req","id":"2","method":"chat.send","params":{
  "sessionKey":"default",
  "message":"Hello!",
  "idempotencyKey":"unique-key-123"
}}
```
Response: `{type:"res", id:"2", ok:true}` (acknowledgment)

**Receiving streaming response:**
```json
// Delta (partial):
{"type":"event","event":"chat","payload":{
  "state":"delta",
  "message":{"content":[{"text":"Hey "}]}
}}
// Final:
{"type":"event","event":"chat","payload":{
  "state":"final",
  "message":{"content":[{"text":"Hey there! 🐾"}]}
}}
```

### Other Useful Methods
- `health` — Check gateway health
- `status` — Get system status
- `chat.sessions` — List chat sessions
- `chat.history` — Get chat history
- `cron.list` — List scheduled tasks
- `channels.status` — Get channel status

### WebSocket Implementation Notes
- **Buffer size**: Use 8192+ bytes (hello-ok response can be >5KB)
- **Task stack**: 8192 bytes minimum (ED25519 signing uses significant stack)
- **Fragment reassembly**: Large messages arrive in multiple DATA events; check `payload_len` vs `data_len`
- **Heartbeats**: Server sends `tick` events and `health` events periodically

## Local Reference
The OpenClaw source is cloned at: `C:\Users\Omer\Dropbox\ESP2026\openclaw`

Key source files:
- Gateway WebSocket protocol: `src/gateway/`
- Device auth: `src/gateway/ws/handlers/connect.ts`
- Frame schema: `src/gateway/ws/frame-schema.ts`
- Client validation: `src/gateway/ws/clients.ts`

## Keeping OpenClaw Up to Date
```bash
cd C:\Users\Omer\Dropbox\ESP2026\openclaw
git pull origin main
```
