# 豆包 AI 桌面对话机器人 设计文档

- 日期：2026-08-16
- 硬件：Waveshare ESP32-S3-Touch-AMOLED-2.06（ESP32-S3、32MB Flash、8MB PSRAM、SH8601 AMOLED 410×502、FT3168 触摸、ES8311 DAC + ES7210 ADC、SD 卡槽）
- 云端：火山引擎 豆包语音·端到端实时语音（全双工版本），模型代号 Seeduplex，`model = 1.2.6.1`
- 骨架来源：AIWatch_Ver2.0（同板卡已验证的成熟工程，IDF v5.5.5 + LVGL 9.5.0）

## 1. 已确认的关键决策

| # | 决策点 | 结论 |
|---|---|---|
| 1 | API 形态 | 豆包实时语音**单 WSS 全双工**（ASR+LLM+TTS 一条连接内完成），不采用三接口组合 |
| 2 | 唤醒方式 | 触摸单击 + BOOT 按键 + 语音唤醒词"你好小智"（ESP-SR WakeNet） |
| 3 | 主界面 | 对话气泡列表（用户/机器人气泡、流式刷新、可滚动回看本轮） |
| 4 | 设置入口 | 设备轻设置页（音量/清空对话/设备信息/WiFi 状态）+ webserver 网页重配置（WiFi 凭据/API Key/音色/提示词） |
| 5 | 范围 | 保留 [DEVICE:] 语音控制、聊天记录 SD 落盘、MP3 播放、连续对话自动续听；摄像头/深睡省电裁掉；OTA 仅预留接口 |

## 2. 总体架构

```
AIWatch_Doubao/  (从 AIWatch_Ver2.0 复制后改造)
├── main/
│   ├── app_main.c         搬+改：启动序列（SNTP 改为可选，非鉴权硬依赖）
│   ├── app_state.c/h      搬+改：共享状态/事件位/响应处理/[DEVICE:] 指令解析
│   ├── app_state_machine  搬+改：对话期允许聆听+播报同时（全双工仲裁）
│   ├── app_tasks.c        搬+改：状态看门狗/重连退避/待机屏保任务
│   ├── doubao_chat.c      新写（替换 voice_chat.c）：对话编排（VAD、上行帧循环、打断）
│   ├── serial_cmd.c       搬+改：串口 CLI（talk/say/reboot + doubao 状态命令）
│   └── mem_monitor.c      搬
├── components/
│   ├── board              直接搬（AMOLED-2.06 目标已完整适配）
│   ├── doubao_voice       新写：豆包 WSS 全双工协议客户端
│   ├── ui                 重做：对话气泡列表 + 状态胶囊 + 轻设置页 + 屏保
│   ├── wifi_manager       直接搬（STA 重连 + AP 配网 + captive portal）
│   ├── settings           搬+改：api_key/voice/speed/loudness/system_prompt/volume/auto_continue
│   ├── webserver          搬+改：加"豆包配置"页
│   ├── wake_word          直接搬（"你好小智"，与对话 mic 做 pause/resume 仲裁）
│   ├── mp3_player + esp_audio_* + gmf_*   直接搬（与播报互斥仲裁已有）
│   ├── notes_manager      直接搬（对话记录 SD 落盘）
│   └── error_log          直接搬
└── (删除：openclaw、stt、tts、sscma_client、ed25519_lib、摄像头相关代码)
```

- 依赖方向：main → components；跨任务通信走 FreeRTOS Event Group + 回调注册；board 是唯一碰硬件引脚的组件。
- 分区表 / IDF / LVGL 全沿用 Ver2.0：app 8MB + model SPIFFS 960KB（WakeNet）+ storage SPIFFS 2MB + nvs。
- 内存纪律（PRD 四铁律）：LVGL buffer 内部 RAM；static task（TCB 内部 RAM、栈可 PSRAM）；大缓冲 PSRAM 静态分配；mbedtls 用 Ver2.0 已验证配置。

### 核心变化 vs Ver2.0

| 项目 | Ver2.0 | 新项目 |
|---|---|---|
| 对话链路 | 录音→HTTP STT→私有 WS LLM→HTTP SSE TTS 三段式 | 一条 WSS：doubao_voice 同时管上行音频、下行文本、下行音频 |
| 语音期间 mic | 录音独占（播报时 mic 关闭） | 全双工：播报时 mic 仍开，用户开口即打断 |
| 上下文 | 客户端拼 messages | 服务端托管（20 轮），客户端只存 session.id |
| UI | 竖屏手表单屏 | 横屏桌面机器人气泡列表 |

## 3. 音频链路（全双工）

```
麦克风 ES7210 ──I2S RX 16k/16bit──> 采集任务 ──640样本(40ms)/帧──> Base64 ──> input_audio_buffer.append
扬声器 ES8311 <──I2S TX 16k/16bit── 播放任务 <──重采样24k→16k── 播放环形缓冲(PSRAM) <── output_audio.delta(Base64)
```

- **I2S 总线统一 16kHz**（ES8311/ES7210 共享帧时钟，收发同采样率），匹配豆包上行要求；下行 24k PCM → 线性插值重采样到 16k（复用 Ver2.0 TTS 重采样代码）。
- **首版 PCM 双向**（上行 ≈42KB/s、下行 base64 ≈64KB/s，WiFi 无压力）；Opus 编解码为后续优化项。
- 采集/播放双任务 + esp_codec_dev 读写 mutex。
- 唤醒词仲裁：待机时 WakeNet 独占 mic；对话期 `wake_word_pause()`；回待机 `resume()`。
- 本地 VAD：移植 Ver2.0 自适应噪声底算法 → 静音 1.5s 或最长 15s（可配）→ `input_audio_buffer.commit`。

## 4. doubao_voice 组件设计

替换 stt/tts/openclaw 三件套。协议依据：`PRD/Project_Resources/豆包语音_端到端实时语音-全双工版本*.pdf/md`。

### 4.1 对外接口（保留旧接口语义，main 改动最小）

```c
doubao_init(cfg)                  // cfg: api_key, model(1.2.6.1), instructions, voice, speed, loudness
doubao_connect() / disconnect()
doubao_send_audio(pcm16, len)    // 上行帧（对应旧 stt_upload_chunk）
doubao_commit_audio()             // 判停（对应旧 stt_finalize）
doubao_interrupt()                // 打断：response.cancel + 停本地播报
doubao_clear_session()            // 清空对话：session.close + 重建
// 回调：on_transcript(delta/final) on_output_text(delta/done) on_audio_delta(pcm24)
//       on_state_changed on_error on_session_id
```

### 4.2 内部结构

- `ws_client.c`：esp_websocket_client 封装。WS ping 1s→认证后 30s（照搬 ws-auth-ping-fix 教训）；断线指数退避 2→60s 重连。
- `protocol.c`：事件编解码。上行 JSON 用静态缓冲构造；下行流式 cJSON 逐帧解析不攒包。
- 两个消费者队列：`text_evt_q`、`audio_evt_q`（PSRAM 静态环形缓冲）。

### 4.3 协议要点

- 端点：`wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue`，鉴权仅 `X-Api-Key` 请求头。
- 握手：`session.create`（model=`1.2.6.1`、instructions=系统提示词、audio input pcm 16k / output pcm 24k、voice/speed/loudness 取自 settings）→ `session.created` 取 `session.id`。
- 重连续接：重连后 `session.create` 带上次 `session.id` 尝试恢复上下文；失败则新建会话并 UI 提示"上下文已重置"。
- 打断：服务端 `input_audio_transcription.started`（用户开口首字）→ 本地停播 + 发 `response.cancel`；触摸双击同样触发。
- 退出意图：`response.output_audio.done` 的 `status_code=20000002` → 回待机。
- 内容安全：`tts_type=audit_content_risky` → 不播报、气泡显示安全提示。
- 清空对话：`session.close` + 重建会话；同时清本地气泡与 SD 记录。
- 心跳：协议无官方心跳 → WS ping 保活 + 空闲超时重建。

## 5. UI 与触摸交互

### 5.1 主界面：对话气泡列表

- 屏幕：原生 410×502 竖屏，**UI 按横屏桌面布局设计，用 LVGL display rotation 转 90° 呈现**（M1 实测后确认旋转方向与触摸坐标映射）。
- 布局：顶部状态条 + 中部可滚动气泡区 + 底部提示文字。
- 气泡：用户右对齐（深色底）、机器人左对齐（浅色底），lv_obj 动态创建进 scroll 容器，流式追加文本，自动滚到底部；思考中显示省略号动画气泡。
- 状态胶囊：待机/聆听/思考/播报/重连中/错误 + WiFi 信号 + 音量图标。
- 本轮历史可滚动回看；notes_manager SD 落盘，重启后最近一轮恢复显示。

### 5.2 触摸手势

| 手势 | 动作 |
|---|---|
| 单击 | 待机→唤醒聆听；播报中→打断并进入聆听 |
| 双击 | 停止播报（response.cancel + 停本地播放） |
| 长按 1s | 进入轻设置页 |

### 5.3 轻设置页（长按进入）

音量滑块 / 清空对话按钮 / WiFi 状态（SSID+IP）/ 设备信息（固件版本、会话状态）/ 提示"WiFi 密码、API Key 等配置请浏览器访问 http://&lt;IP&gt;"。

### 5.4 屏保（防烧屏）

待机 60s 无活动 → 暗色时钟 + 亮度 30% + 每 30s 微移像素；任意触摸/按键/唤醒词退出。

### 5.5 中文字体

- lv_font_conv 从 NotoSansSC 生成：GB2312 一级字库 3755 常用字 + ASCII + 常用标点。
- 三档：12pt（状态栏）/ 16pt（气泡正文）/ 24pt（大字号提示），估算 ~800KB flash。
- 照搬 Ver2.0 `fonts/regenerate_font.sh` 流程；不可渲染字符降级为"□"。

### 5.6 webserver 网页（复用改造）

设置页加 API Key / 音色 voice / 语速 speed / 音量 loudness / 系统提示词输入项（settings NVS，secret 打码）；配网流程不变（AP captive portal）。

## 6. 对话数据流（含打断）

```
待机(唤醒词监听+屏保)
  │ 唤醒词 / 单击
  ▼
唤醒：wake_word_pause() → 确保 WSS 已连接 → 进入聆听态
  ├─ 采集任务 40ms/帧 → doubao_send_audio
  ├─ transcription.delta → 气泡实时显示用户说话
  ├─ 本地 VAD 静音1.5s 或 15s上限 → doubao_commit_audio()
  ▼
模型处理：output_text.delta 流式 → 机器人气泡逐字刷新
  ▼
播报：output_audio.delta → 24k→16k 重采样 → 环形缓冲 → 扬声器
  ├─ 播报中用户开口：transcription.started → 停播 + response.cancel → 回聆听态
  ├─ 双击：同上打断
  ├─ response.done → 本轮结束
  │     ├─ 自动续听(开)：保持聆听，空闲 8s（idle_timeout 可配）→ 回待机
  │     └─ 退出意图(20000002)：回待机
  ▼
待机：wake_word_resume() → 屏保倒计时
```

## 7. 错误处理

| 场景 | 处理 |
|---|---|
| WSS 断线 | 指数退避 2→60s 重连；UI"重连中"；重连带 session.id 续接，失败则新建会话并提示 |
| API 错误事件 | error_log + UI 气泡提示；鉴权失败→提示检查 API Key |
| 限流/冷却 | 冷却 5s 自动恢复，UI 提示"稍后重试" |
| 音频设备异常 | codec 读写错误 → 重初始化音频子系统，不崩溃 |
| 内存保护 | 状态超时看门狗（思考30s/播报60s 强制回收）+ 任务看门狗 + mem_monitor |
| API 超时 | 对话进行中 10s（可配）未收到任何下行事件 → 判定超时重连 |

## 8. 测试

- 集成测试清单（照搬 PRD integration-test 方法）：启动序列日志验收（Board→I2C→codec→WiFi→WSS→Ready）→ 单轮对话 → 打断 → 连续多轮 → 清空对话 → 断网重连 → 唤醒词。
- PC 音频模拟（PRD audio-emulation 方法）做自动化语音回放测试。
- 串口 CLI：`talk`（触发对话）、`doubao status`（会话/连接状态）、`say <文本>`（speech_text_buffer 直推）、`reboot`。
- 稳定性：2 小时连续运行 + mem_monitor 内存曲线（无泄漏无崩溃）。

## 9. 里程碑

| 阶段 | 内容 | 验收 |
|---|---|---|
| M1 | 骨架移植：复制裁剪编译烧录，board+UI+音频验证 | 屏幕点亮、播放测试音、mic 采到音 |
| M2 | doubao_voice 协议链路：WSS 连接、session 管理、文本流式收发 | `say` 推送文本，气泡流式显示 |
| M3 | 完整语音链路：上行 PCM+VAD、播报、打断、唤醒词集成、全双工 | 完整语音对话 + 打断可用 |
| M4 | UI 完善：气泡、设置页、屏保、中文字体、错误处理 | FR-09~14 全验收 |
| M5 | 测试打磨：集成/异常/稳定性测试、文档 | 验收标准表全通过 |

## 10. 需求覆盖对照

| 需求 | 方案 |
|---|---|
| FR-01 唤醒 | 单击/按键/唤醒词（§5.2、§3） |
| FR-02 采集 | 16k/16bit/mono（§3） |
| FR-03 VAD | 静音 1.5s / 15s 上限可配（§3） |
| FR-04/05 ASR+对话 | 豆包 WSS 端到端一体（§4） |
| FR-06 TTS | output_audio.delta 播报（§3） |
| FR-07 打断 | transcription.started + response.cancel（§4.3、§6） |
| FR-08 上下文 | 服务端 20 轮托管 + 自动续听（§4.3、§6） |
| FR-09~12 气泡/状态/流式/手势 | §5.1、§5.2 |
| FR-13 设置页 | 轻设置页 + 网页（§5.3、§5.6） |
| FR-14 屏保 | §5.4 |
| FR-15 配网 | wifi_manager AP captive portal（沿用） |
| FR-16 重连 | §7 |
| FR-17 API Key NVS | settings + webserver（§5.6） |
| FR-18 系统提示词 | session.instructions（网页可配） |
| FR-19/20 上下文裁剪/清空 | 服务端托管 + clear_session（§4.3） |
| FR-21 内容安全 | audit_content_risky 处理（§4.3） |
| FR-22 音量 | 轻设置页 + [DEVICE:] 指令 |
| FR-23 异常恢复 | §7 |
| FR-24 OTA | 预留接口（v1 不实现） |
| FR-25 串口日志 | serial_cmd 沿用 |

## 11. 风险与后续优化

1. 豆包完整错误码表不在所给 PDF 内——v1 通用错误处理，遇到具体错误码再细化。
2. Opus 编解码（上行 speech_opus / 下行 ogg_opus）可省带宽，v2 优化项。
3. 全双工期间 ES8311 播放 + ES7210 采集同时工作的稳定性需在 M3 实测验证（Ver2.0 未验证过该组合）。
4. API Key 凭据安全：NVS 存储 + 网页输入（无明文硬编码），满足 FR-17 底线。
