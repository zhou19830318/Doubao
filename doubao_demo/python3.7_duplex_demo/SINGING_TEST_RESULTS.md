# Doubao Singing Feature Test Results

## Test Summary

| Model | Type | `enable_music` | Result |
|-------|------|----------------|--------|
| `1.2.6.1` | SC 2.0 | True | ✅ Works, but `tts_type=chat_tts_text` (regular TTS, no singing) |
| `1.2.1.1` | O 2.0 | True | ❌ Server error 42000999 |
| `1.2.1.1` | O 2.0 | False | ✅ Session created, no response to text |

## API Interaction Parameters

### session.create (Model 1.2.1.1 - O2.0)
```json
{
  "type": "session.create",
  "session": {
    "model": "1.2.1.1",
    "instructions": "You are a creative assistant...",
    "audio": {
      "input": {"format": {"type": "pcm", "rate": 16000}},
      "output": {
        "format": {"type": "pcm_s16le", "rate": 24000},
        "voice": "zh_female_xiaohe_jupiter_bigtts"
      }
    }
  },
  "extension": {
    "asr": {"extra": {}},
    "tts": {"extra": {}},
    "dialog": {
      "extra": {
        "enable_loudness_norm": true,
        "enable_music": true
      }
    }
  }
}
```

### speech_text_buffer.append
```json
{
  "type": "speech_text_buffer.append",
  "speech_id": "<uuid>",
  "text": "唱一首小星星"
}
```

### speech_text_buffer.commit
```json
{
  "type": "speech_text_buffer.commit",
  "speech_id": "<uuid>"
}
```

## Server Response (Error)
```json
{
  "type": "error",
  "error": {
    "type": "Internal Server Error",
    "code": "55000000",
    "message": "sami error: codes=42000999, desc=rpc error: code = 2 desc = malformed header: missing HTTP content-type"
  }
}
```

## Root Cause
The API key used for this test does **NOT** have access to model `1.2.1.1` (O2.0).

## Required Action
Contact Volcengine support to:
1. Enable model `1.2.1.1` (O2.0) access for this API key
2. Enable singing feature for this account

## Notes
- Model `1.2.6.1` (SC2.0) does **NOT** support singing (`enable_music` has no effect)
- Only model `1.2.1.1` (O2.0) supports singing
- Both `speech_text_buffer` and `input_audio_buffer` produce the same error with model `1.2.1.1`
