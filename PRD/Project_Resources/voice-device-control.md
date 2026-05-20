# Voice Device Control

## Overview
OpenClaw can control device settings via `[DEVICE:key=value]` command tags embedded in chat responses. The device parses these commands, executes them, and strips the tags from the displayed/spoken response.

## Protocol
- Format: `[DEVICE:key=value]` on its own line in the response
- Multiple commands can appear in one response
- Commands are parsed and executed in `parse_device_commands()` in `main/app_state.c`
- Tags are removed from the response buffer via `memmove()` before display/TTS

## Supported Commands
| Command | Values | Effect |
|---------|--------|--------|
| `volume` | 0-100 | Set speaker volume, save to NVS |
| `brightness` | 0-100 | Set display brightness, save to NVS |
| `rgb` | `rainbow`, `aurora`, `starfield`, `fire`, `ocean`, `off`, `on`, `R,G,B` | Control RGB LED animation or color |
| `sleep` | minutes | Set sleep timeout in minutes |
| `webserver` | `on`, `off` | Toggle web server |
| `auto_read` | `on`, `off` | Toggle auto-read response |
| `reboot` | (any) | Reboot device after 2s delay |

## OpenClaw Prompt
All 3 PREFIX prompt strings in `openclaw_client.c` include device command instructions:
- Main chat PREFIX (~line 983)
- Image chat PREFIX (~line 1080)
- Short-response/audio PREFIX (~line 1265)

The prompt tells OpenClaw:
1. Available commands with valid ranges
2. To include commands on separate lines
3. Example: user says "turn volume to 80" → response includes `[DEVICE:volume=80]`

## TTS Sanitization
Before JSON-encoding text for the TTS server:
1. **Unicode sanitization**: em-dash→`-`, en-dash→`-`, smart quotes→ASCII, ellipsis→`...`, NBSP→space
2. **JSON escaping**: `\n`→`\\n`, `\r`→`\\r`, `\t`→`\\t`, other control chars stripped
3. Located in `tts_speak()` in `components/tts/tts_client.c`

## Files
- `main/app_state.c` — `parse_device_commands()` function
- `components/openclaw/openclaw_client.c` — PREFIX prompt strings with command instructions
- `components/tts/tts_client.c` — Unicode sanitization + JSON escaping
