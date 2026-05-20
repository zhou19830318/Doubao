#pragma once

#define SECRETS_WIFI_SSID     "Prefoco"
#define SECRETS_WIFI_PASSWORD "18961210318"

#define SECRETS_OPENCLAW_HOST  "118.25.20.250"
#define SECRETS_OPENCLAW_PORT  18789
#define SECRETS_OPENCLAW_TOKEN "6f1a78bcbd3ed914ca67ef51064f3ade3fb15f231085ef89"

/* DashScope (百炼) API — for STT */
#define SECRETS_DASHSCOPE_API_KEY "sk-51215877220940cea36363fcea22d431"
#define SECRETS_STT_MODEL         "fun-asr-realtime-2026-02-28"
#define SECRETS_STT_ENDPOINT      "wss://dashscope.aliyuncs.com/api-ws/v1/inference/"

/* MiMo TTS (Xiaomi) */
#define SECRETS_MIMO_API_KEY  "sk-cv5bjp19wpp9wkxnd6w9btxy29g48gnk5cuj6ul1udaby5ds"
#define SECRETS_MIMO_ENDPOINT "https://api.xiaomimimo.com/v1/chat/completions"
#define SECRETS_MIMO_MODEL    "mimo-v2-tts"
#define SECRETS_MIMO_VOICE    "mimo_default"

/* Device identity — leave empty to auto-generate, or set 64 hex chars for ED25519 */
#define SECRETS_DEVICE_KEY_HEX ""