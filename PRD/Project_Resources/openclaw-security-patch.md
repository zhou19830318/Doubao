# OpenClaw LAN Security Patch

## Problem
OpenClaw's `isSecureWebSocketUrl()` blocks ALL `ws://` connections to non-loopback addresses, preventing:
- `openclaw tui` from working even on the same machine (when gateway binds to LAN IP)
- WebUI model management from other LAN machines
- Any ws:// connection to private network addresses

## Root Cause
- File: `src/gateway/net.ts` → compiled to `dist/net-COi3RSq7.js`
- The function only allows `wss://` or `ws://127.0.0.1/localhost`
- No config option exists to bypass this check
- The same file has `isPrivateOrLoopbackAddress()` but it's NOT used in the security check

## Fix: Patch Installed JS Bundle
All callers import from a single module file. Patch that one file.

### Location on Server
```
/home/user/.npm-global/lib/node_modules/openclaw/dist/net-COi3RSq7.js
```

### What to Change
In `isSecureWebSocketUrl()`, after the loopback check, add RFC1918 private IP acceptance:

```javascript
// Replace: return isLoopbackHost(parsed.hostname);
// With:
const h = parsed.hostname.trim().toLowerCase();
if (isLoopbackHost(h)) return true;
const unbracket = h.startsWith("[") && h.endsWith("]") ? h.slice(1, -1) : h;
const parts = unbracket.split(".").map(Number);
if (parts.length === 4 && parts.every(p => !isNaN(p))) {
  if (parts[0] === 10) return true;
  if (parts[0] === 172 && parts[1] >= 16 && parts[1] <= 31) return true;
  if (parts[0] === 192 && parts[1] === 168) return true;
}
return false;
```

### Automated Script
Use `tools/patch_oc_security.py` to apply the patch:
```bash
python3 tools/patch_oc_security.py
```

### ⚠️ WARNING
- OpenClaw npm updates (`npm update -g openclaw`) will OVERWRITE this patch
- Re-run the patch script after any OpenClaw update
- A backup `.bak` file is created automatically

## TUI Device Pairing
After fixing the security check, TUI may need pairing approval:
- Paired devices: `~/.openclaw/devices/paired.json`
- Pending: `~/.openclaw/devices/pending.json`
- Use `tools/approve_tui.py` or manually edit `paired.json` to set `operator.admin` scope
- Restart gateway after changes: `pkill openclaw-gatew && nohup openclaw gateway --port 18789 > /tmp/oc.log 2>&1 &`

## SSH Access
```powershell
& "C:\Program Files\PuTTY\plink.exe" -ssh -batch <SSH_USER>@<OC_HOST> -pw <SSH_PASSWORD> '<command>'
```
