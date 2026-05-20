# OpenClaw Chat Response Handling

## Chat Protocol Flow
1. Client sends `chat.send` RPC (type: "req") with `sessionKey`, `message`, `idempotencyKey`
2. Server responds with `res` confirming receipt (includes `runId`)
3. Server sends `agent` events (lifecycle start, assistant stream)
4. Server sends `chat` events with `state`:
   - `"delta"` — streaming text chunks (partial response)
   - `"final"` — complete response with full message
   - `"error"` — error with `errorMessage`

## CRITICAL: Response Buffer Deduplication
The `delta` events contain incremental text AND the `final` event contains the **complete** message.
If you append both, you get doubled text (e.g., "AllAll good!").

**Fix**: When processing the `final` event, **REPLACE** the response buffer with the final message content, do NOT append to existing streaming data.

```c
// CORRECT: Replace buffer on final
if (strcmp(state, "final") == 0) {
    s_oc.response_buf[0] = '\0';
    s_oc.response_len = 0;
    // Then copy final message content
}

// WRONG: Append to existing buffer
if (strcmp(state, "final") == 0) {
    // Appends to streaming data → doubled text
    memcpy(s_oc.response_buf + s_oc.response_len, text, tlen);
}
```

## State Overwrite Guard
After the chat response callback fires, `set_state(OPENCLAW_STATE_CONNECTED)` is called,
which triggers `on_openclaw_state()`. This must NOT override UI_STATE_RESPONSE with UI_STATE_IDLE.

**Guard pattern**:
```c
case OPENCLAW_STATE_CONNECTED:
    ui_state_t cur = ui_get_state();
    if (cur <= UI_STATE_CONNECTING || cur == UI_STATE_ERROR) {
        set_app_state(UI_STATE_IDLE);
    }
    // Otherwise: leave UI in RESPONSE/TTS state
```

## Emoji Stripping
OpenClaw responses often include emojis (🐾, ✅, 🕦) that render as rectangles in Montserrat font.
`ui_sanitize_text()` strips 3-byte and 4-byte UTF-8 sequences (emoji range), keeping ASCII and 2-byte Latin characters.

## Short Response Prompting
Every chat message is prefixed with an instruction to answer in 1-3 words:
```
[You are responding to a tiny wearable screen.
Answer in 1-3 words ONLY (e.g. OK, Done, Yes, No, Problem, Working on it).
If you need to explain more, the user will ask for details.]
```

This works well — tested responses: "4", "Paris", "All good!", "Doing great!", "HeyHey!"

## Response Color Coding
- Green: OK, Done, Yes, Sure, Ready
- Red: No, Error, Problem, Failed
- Orange: Working, Busy, Wait
- Default (white/cyan): All other responses
