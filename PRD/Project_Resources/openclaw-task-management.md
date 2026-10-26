# OpenClaw Task Management Architecture

## Overview
OpenClaw has **structured task management** — processes are NOT just raw bash loops. The framework uses a session-based registry with unique IDs, PID tracking, lifecycle management, and TTL-based cleanup.

## Architecture (Source Code)

### Process Registry (`src/agents/bash-process-registry.ts`)
- **Map-based registry**: `runningSessions` (active) + `finishedSessions` (completed)
- Each process has:
  - `id`: Unique session ID (slug-based)
  - `pid`: OS process ID (tracked for kill)
  - `command`: Shell command being run
  - `status`: "running" | "completed" | "failed" | "killed"
  - `backgrounded`: Whether running in background
  - `exitCode`, `exitSignal`: Termination info
  - `aggregated`: Full output buffer
  - `tail`: Last 2000 chars of output
- Auto-cleanup via TTL sweeper (default 30min, configurable via `PI_BASH_JOB_TTL_MS`)

### Process Supervisor (`src/process/supervisor/supervisor.ts`)
- `spawn()`: Creates `ManagedRun` with unique `runId`, `sessionId`, `scopeKey`
- `cancelScope(scopeKey)`: Kills ALL tasks in a scope (for abort/cleanup)
- Termination reasons: `manual-cancel`, `overall-timeout`, `no-output-timeout`, `user-abort`

### Exec Runtime (`src/agents/bash-tools.exec-runtime.ts`)
- `background` param: Run immediately in background
- `yieldMs`: Auto-background after N milliseconds (default 10000ms)
- `notifyOnExit`: Post system event on completion
- Exit notification includes: session ID (8-char prefix), exit code/signal, output tail

## Gateway API Methods Available to HeyClawy

### Directly Available (JSON-RPC over WebSocket)

| Method | Purpose | Key Response Fields |
|--------|---------|-------------------|
| `health` | Gateway health + session summary | `sessions: {count, recent: [{key, updatedAt, age}]}` |
| `sessions.list` | List all sessions | `{path, count, recent: [{key, updatedAt, age}]}` |
| `sessions.preview` | Get session transcript snippets | Transcript data |
| `cron.list` | List scheduled cron jobs | `{jobs: [{id, name, enabled, ...}]}` |
| `cron.status` | Cron service status | Running/stopped info |
| `chat.abort` | Abort running chat session | Cancel active request |

### NOT Available as Gateway Methods
- `process list/kill/poll/log` — These are **agent tools**, not gateway API methods
- Direct process registry query — No gateway endpoint
- PID lookup — Only internal to agent execution

## How HeyClawy Should Track Activity

### 1. WebSocket Events (Real-time, already implemented)
- **Agent events**: `agent.lifecycle.start/end`, `agent.tool.*`, `agent.assistant.*`
- **Cron events**: `cron.job.start/finish` with job name
- **Chat events**: `chat.delta/final/error/aborted`

### 2. Health Polling (Periodic)
- `sessions.count`: Total session count
- `sessions.recent`: Recently active sessions with `age` in ms
- `last_activity_sec`: Derived from `sessions.recent[0].age`

### 3. Sessions List (NEW — should implement)
- Call `sessions.list` for more detailed session information
- Shows all sessions with timestamps, useful for the Tasks screen

## Implementation in HeyClawy

### Current State
- Device receives WebSocket events for real-time activity tracking ✅
- Device polls `health` every 5-30s for session/activity fallback ✅  
- Cron jobs listed and displayed in Tasks screen ✅
- External activity detection and elapsed timer ✅

### Recommended Additions
1. Add `sessions.list` gateway call for richer session data
2. Show session count in status bar or tasks screen header
3. Use `sessions.recent[].age` for more accurate "last activity" detection

## Key Insight
The user's concern about "raw bash while true loops" refers to **cron job payloads** (user-defined scripts), not OpenClaw's execution framework. OpenClaw wraps ALL executed commands in supervised processes with PID tracking, output capture, and cleanup. The gap is that the gateway API doesn't expose the process registry directly — only session-level data.
