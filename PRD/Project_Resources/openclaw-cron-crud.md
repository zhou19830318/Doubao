# OpenClaw Cron (Background Tasks) CRUD API

## Overview
OpenClaw supports scheduled/periodic background tasks via the cron subsystem. HeyClawy can list, add, update, toggle, and remove cron jobs via JSON-RPC over WebSocket.

## Required Scope
- `operator.admin` — required for all cron write operations (add, update, remove)
- Adding this scope triggers a re-pairing request on the OC server

## API Methods

### cron.list
Lists all cron jobs. By default only returns enabled jobs.
```json
{"method": "cron.list", "params": {"includeDisabled": true}}
```
Response key: `"jobs"` (NOT "crons")

### cron.add
Creates a new scheduled job. Two kinds: `agentTurn` (sends message to agent) and `systemEvent` (sends system event).
```json
{
  "method": "cron.add",
  "params": {
    "name": "My Task",
    "kind": "agentTurn",
    "message": "Do something",
    "schedule": {"kind": "every", "ms": 10000},
    "delivery": "announce",
    "agentId": "main"
  }
}
```
**⚠️ CRITICAL**: For `agentTurn` kind, the payload field is `"message"` NOT `"text"`. `systemEvent` kind uses `"text"`.

### cron.update
Updates a cron job. Uses patch semantics.
```json
{"method": "cron.update", "params": {"id": "<uuid>", "patch": {"enabled": false}}}
```

### cron.remove
Removes a cron job by ID.
```json
{"method": "cron.remove", "params": {"id": "<uuid>"}}
```

### cron.run
Manually triggers a cron job immediately.
```json
{"method": "cron.run", "params": {"id": "<uuid>"}}
```

### cron.runs
Gets execution history for a job.
```json
{"method": "cron.runs", "params": {"id": "<uuid>"}}
```

## Job Object Structure
```json
{
  "id": "e8040f14-acb0-4ed0-b5f0-1ca5606d0d02",  // 36-char UUID
  "name": "HeyClawy Test",
  "kind": "agentTurn",
  "message": "Test message",
  "enabled": true,
  "schedule": {"kind": "every", "ms": 10000},
  "delivery": "announce",
  "agentId": "main",
  "state": {
    "runningAtMs": 0,           // >0 means currently running
    "lastRunAtMs": 1749915023000,
    "lastStatus": "error",       // "ok" or "error"
    "lastError": "cron announce delivery failed",
    "lastDurationMs": 1510,
    "consecutiveErrors": 4,
    "totalRuns": 7
  }
}
```

## Implementation Notes

### UUID Size
Job IDs are 36-char UUIDs. The C struct field must be at least 37 bytes (36 + null). Using `char id[40]` for safety.

### Polling Strategy
- Normal: poll `cron.list` every 30s
- When any task is running (state.runningAtMs > 0): poll every 5s
- Use `tasks_running` count to switch between polling intervals

### Device Pairing for Admin Scope
When adding `operator.admin` scope to the WebSocket connect request:
1. Server requires re-pairing (scope upgrade)
2. Manual approval: SSH to `<SSH_USER>@<OC_HOST>` (see secrets.txt for credentials)
3. Edit `~/.openclaw/devices/paired.json` — add `operator.admin` to device's scopes and token scopes
4. Clear `~/.openclaw/devices/pending.json`
5. Kill gateway process: `kill <pid>` (auto-restarts)

### Web API Endpoints
- `GET /api/tasks` — returns JSON array of all cron jobs
- `POST /api/tasks/toggle` — body `{"id": "<uuid>", "enabled": true/false}`
- `GET /api/status` — includes `task_count`, `tasks_running`, `tasks_active`

### RGB LED Indicator
When bg tasks are running during IDLE state: subtle teal breathe (0, 8, 12).
Reverts to normal green when no tasks running.

### Serial Commands
- `tasks` — show all cron jobs in serial
- `cron-add-test` — create dummy test job (fetch google.com every 10s)
- `cron-remove <id>` — remove a job by UUID
- `abort` — abort current chat or running operation
