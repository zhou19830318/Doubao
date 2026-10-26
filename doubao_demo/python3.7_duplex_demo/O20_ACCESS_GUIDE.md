# O2.0 (Model 1.2.1.1) API 访问指南

## 问题分析

模型 `1.2.1.1` (O2.0) 使用 **不同的端点和认证方式**：

| 项目 | JSON WebSocket (当前使用) | O2.0 Binary Protocol |
|------|---------------------------|----------------------|
| 端点 | `wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue` | `wss://openspeech.bytedance.com/api/v3/realtime/dialogue` |
| 认证 | `X-Api-Key` | `X-Api-App-ID` + `X-Api-AccessKey` + `X-ApiResource-Id` |
| 协议 | JSON | Binary |
| 模型 | 1.2.6.1 (SC2.0) | 1.2.1.1 (O2.0) |

## 需要的认证信息

从火山引擎控制台获取：
1. **X-Api-App-ID**: 应用ID (例如: `123456789`)
2. **X-Api-AccessKey**: 访问密钥 (例如: `your-accesskey`)

这些信息在火山引擎控制台 → 豆包语音 → 应用管理 中可以找到。

## 配置步骤

### 1. 获取认证信息
登录火山引擎控制台: https://console.volcengine.com/
进入: 豆包语音 → 应用管理
找到你的应用，获取 App-ID 和 AccessKey

### 2. 修改 Python 测试脚本
编辑 `test_singing_o20_official.py`，更新：
```python
HEADERS = {
    "X-Api-App-ID": "你的App-ID",
    "X-Api-AccessKey": "你的AccessKey",
    "X-ApiResource-Id": "volc.speech.dialog",
    "X-Api-App-Key": "PlgvMymc7f3tQnJ6",
}
```

### 3. 修改 ESP32 C++ 代码
需要修改 `ws_client.c` 使用正确的端点和认证头：
- 端点: `/api/v3/realtime/dialogue` (不是 `/api/v3/duplex/realtime/dialogue`)
- 认证头: `X-Api-App-ID`, `X-Api-AccessKey`, `X-ApiResource-Id`

## API 交互参数

### StartSession (初始化会话)
```json
{
  "event": "StartSession",
  "session_id": "<uuid>",
  "payload": {
    "tts": {
      "audio_config": {
        "channel": 1,
        "format": "pcm_s16le",
        "sample_rate": 24000
      },
      "speaker": "zh_female_xiaohe_jupiter_bigtts"
    },
    "asr": {
      "extra": {}
    },
    "dialog": {
      "extra": {
        "input_mod": "text",
        "enable_music": true,
        "enable_loudness_norm": true
      }
    },
    "model": "1.2.1.1"
  }
}
```

### TaskRequest (发送文本请求)
```json
{
  "event": "TaskRequest",
  "session_id": "<uuid>",
  "payload": {
    "text": "唱一首小星星"
  }
}
```

### 响应事件
- `SessionStarted`: 会话初始化成功
- `ASRResponse`: 语音识别结果
- `TTSResponse`: TTS音频数据
- `TTSEnded`: TTS结束
- `SessionFinished`: 会话结束

## 下一步

1. 从火山引擎控制台获取 App-ID 和 AccessKey
2. 更新测试脚本中的认证信息
3. 运行测试验证唱歌功能
4. 将成功的配置应用到 ESP32 C++ 代码
