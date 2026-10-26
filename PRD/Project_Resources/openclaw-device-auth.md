# Skill: OpenClaw Device Identity Authentication (ED25519)

## Overview
OpenClaw Gateway requires **device identity** for write operations (e.g., `chat.send`).
Token-only authentication clears all scopes for non-localhost connections.

## Algorithm: ED25519 (NOT ECDSA!)
- The gateway uses **ED25519** (EdDSA), specifically the `crypto.verify(null, ...)` API in Node.js
- mbedTLS 3.6.3 (in ESP-IDF 5.5) does NOT support Ed25519 yet
- We use the `orlp/ed25519` C library (in `components/ed25519_lib/`)

## Key Derivation
1. **Seed**: 32 random bytes (stored as hex in `secrets.h` as `SECRETS_DEVICE_KEY_HEX`)
2. **Keypair**: `ed25519_create_keypair(pubkey, privkey, seed)` → 32-byte pubkey + 64-byte expanded privkey
3. **Device ID**: `SHA-256(raw_32_byte_pubkey).hex()` → full 64-char hex string
4. **Public Key encoding**: `base64url(raw_32_byte_pubkey)` (no padding)

## Auth Payload Format (v2)
```
v2|{deviceId}|{clientId}|{clientMode}|{role}|{scopes}|{signedAtMs}|{token}|{nonce}
```
Example:
```
v2|e62fef0a5685998e...|gateway-client|cli|operator|operator.read,operator.write|1771614996000|f6a42234...|uuid-nonce
```

## Signature
- Sign the UTF-8 encoded auth payload with ED25519: `ed25519_sign(sig, payload_bytes, payload_len, pubkey, privkey)`
- Encode signature as `base64url(64_byte_signature)` (no padding)

## Connect Request
```json
{
  "type": "req", "id": "1", "method": "connect",
  "params": {
    "minProtocol": 3, "maxProtocol": 3,
    "client": {"id": "gateway-client", "version": "0.2.0", "platform": "esp32s3", "mode": "cli"},
    "role": "operator",
    "scopes": ["operator.read", "operator.write"],
    "auth": {"token": "..."},
    "device": {
      "id": "sha256hex_of_pubkey",
      "publicKey": "base64url_pubkey",
      "signature": "base64url_signature",
      "signedAt": 1771614996000,
      "nonce": "uuid_from_challenge"
    }
  }
}
```

## Device Pairing
- First connection with a new device key returns `NOT_PAIRED` error with a `requestId`
- Pair the device using `tools/test_openclaw_auth.py` (runs on PC, auto-approves)
- Once paired, the device can connect forever with the same keypair
- Pairing approval requires `device.pair.approve` from an operator (Control UI or token from localhost)

## Time Requirements
- `signedAt` must be within a few minutes of the server's time
- SNTP must sync BEFORE connecting to OpenClaw
- ESP32 waits for `gettimeofday().tv_sec > 1700000000` (year 2023+) before connecting

## Auth Timing (Verified)
- WebSocket TCP connect: ~100ms
- Challenge event arrives: <10ms after WS connect
- ED25519 sign + build connect request: ~20-40ms
- Connect response arrives: ~60-100ms after request sent
- **Total auth time: ~200ms** (much faster than originally thought)

### Previous "10s delay" myth — DEBUNKED
The perceived 10-second challenge delay was caused by `ESP_LOGD` logging being invisible at default log level. The challenge was always arriving instantly — we just couldn't see it. Switching to `ESP_LOGI` revealed the true <10ms timing.

### "Response never arrives" — caused by stale server connections
When the ESP32 crashes/resets during an active WebSocket session, the server may keep the old connection alive. The new connection then competes with the stale one. A clean server restart or waiting for the server's connection timeout resolves this.

## Stack Size
- ED25519 signing uses ~4KB of stack space
- WebSocket task stack must be ≥8192 bytes

## Files
- `components/ed25519_lib/` — orlp/ed25519 library (C, public domain)
- `components/openclaw/openclaw_client.c` — `send_connect()` with device auth
- `tools/test_openclaw_auth.py` — Python tool for testing auth + pairing
- `tools/heyclawy_device_key.bin` — Persistent 32-byte ED25519 seed (gitignored)
- `main/include/secrets.h` — Contains `SECRETS_DEVICE_KEY_HEX`

## Regenerating Device Key
```bash
# Delete existing key
rm tools/heyclawy_device_key.bin
# Run Python script — generates new key, pairs it automatically
python tools/test_openclaw_auth.py
# Copy the hex from output to secrets.h: SECRETS_DEVICE_KEY_HEX
```
