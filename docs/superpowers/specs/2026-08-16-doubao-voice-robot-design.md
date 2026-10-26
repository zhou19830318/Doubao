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
扬声器 ES8311 <──I2S TX 16k/16bit── 播放任务 <──3:2多相重采样24k→16k── 播放环形缓冲(PSRAM,64KB) <── output_audio.delta(Base64)
```

### 3.1 采样率（已核实，不可省重采样）

- 豆包 API 文档 session 配置示例明确：**上行 `rate:16000`、下行 `rate:24000`**——下行 24k 为协议固定值，不可请求 16k 输出。
- 硬件为**单 I2S 总线**（`board_amoled_206.h`：`BOARD_I2S_NUM=1`，ES8311 TX 与 ES7210 RX 共享 MCLK/BCLK/WS 引脚），收发必须同采样率。
- 决策：**总线保持 16kHz**（唤醒词模型与上行原生 16k，沿用 Ver2.0 已验证配置）；下行 24k→16k 用 **3:2 多相重采样**（上采样×2 半带滤波 + 抽取×3 低通，定点实现）替代纯线性插值；CPU 开销在 M2 实测评估，若超出预算则降级为带限线性插值。

### 3.2 回声处理（分层策略）

- **第一层（v1 必做）播放感知门控**：播报期间本地 VAD/打断阈值动态抬高（以本地播放能量为参考，打断仅在用户能量 > 播报能量 + 6dB 余量时触发）；播报能量由下行 PCM 播放前计算，与上行采集帧对齐比较。
- **第二层（实测验证）服务端回声鲁棒性**：文档未提服务端 AEC；官方 Web demo 为扬声器外放场景，S2S 模型大概率具备一定回声容忍——M2/M3 用回路测试实测确认，并据实调整第一层阈值。
- **第三层（后备，不承诺 v1）**：若门控仍误触发，引入 **esp-sr 的 AEC**（`AEC_MODE_FD_LOW_COST`，线性+NLP，仅支持 16kHz；esp-sr 已是唤醒词依赖，无需新增组件；参考信号取真正送入 I2S 的含音量缩放 PCM 并补偿 ~80ms 声学延迟，CPU 占用需单独评估）。

### 3.3 本地打断检测

播报中第一级打断触发为**本地能量检测**（上行 PCM，动态阈值+自适应噪声底，持续 >80ms）→ 立即本地停播/压低 + 发 `response.cancel`；服务端 `input_audio_transcription.started` 到达后确认状态。触摸双击打断走本地逻辑。阈值计算见 §3.2 第一层。

### 3.4 缓冲与背压

- **上行**：发送队列 12 帧（480ms）深；WS 发送在独立任务，采集任务不阻塞；溢出**丢最旧帧**（早期音频对 ASR 影响最小）并计数告警入 error_log。
- **下行**：播放环形缓冲 64KB PSRAM；欠载补 50ms 渐变静音（防爆音）；过载**丢最旧帧**保证实时性；高水位监控进 mem_monitor。
- **任务优先级**：播放 > 采集 > WS 网络 > UI。

### 3.5 其余

- 首版 PCM 双向（上行 ≈42KB/s、下行 base64 ≈64KB/s，WiFi 无压力）；Opus 编解码为后续优化项。
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

- `ws_client.c`：esp_websocket_client 封装。WS ping 1s→认证后 30s（照搬 ws-auth-ping-fix 教训）；断线指数退避 2→60s 重连，**退避等待中用户唤醒/单击 → 取消退避立即重连一次**（失败再入退避），对话中断线立即重试；重连期间 UI 显示"正在连接"。
- `protocol.c`：事件编解码。上行 JSON 用静态缓冲构造；下行流式 cJSON 逐帧解析不攒包。
- 两个消费者队列：`text_evt_q`、`audio_evt_q`（PSRAM 静态环形缓冲）。

### 4.3 协议要点

- 端点：`wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue`，鉴权仅 `X-Api-Key` 请求头。
- 握手：`session.create`（model=`1.2.6.1`、instructions=系统提示词、audio input pcm 16k / output pcm 24k、voice/speed/loudness 取自 settings）→ `session.created` 取 `session.id`。
- 重连续接：重连后 `session.create` 带上次 `session.id` 尝试恢复上下文；失败则新建会话并 UI 提示"上下文已重置"。
- 打断：第一级为本地能量检测（§3.3），第二级为服务端 `input_audio_transcription.started` 确认；触摸双击/单击走本地逻辑 + `response.cancel`。
- 退出意图：`response.output_audio.done` 的 `status_code=20000002` → 回待机。
- 内容安全：`tts_type=audit_content_risky` → 不播报、气泡显示安全提示。
- 清空对话：先 `interrupt()` 停止本地音频与响应 → `session.close` → 重建会话 → 清本地气泡与 SD 记录（顺序保证无竞态）。
- 心跳：协议无官方心跳 → WS ping 保活 + 上行发送成功监测连接健康（聆听态）。

### 4.4 协议附录（事件序列与消息示例）

**典型单轮对话事件序列**：

```
客户端: session.create        {session:{model:"1.2.6.1", instructions:"<系统提示词>",
                                       audio:{input:{format:{type:"pcm",rate:16000}},
                                              output:{format:{type:"pcm",rate:24000}, voice:"<音色>"}}}}
服务端: session.created       {session:{id:"<session_id>"}}     ← 保存 id 用于重连续接
客户端: input_audio_buffer.append ×N   {type:"input_audio_buffer.append", audio:"<Base64 PCM16k>"}
客户端: input_audio_buffer.commit      {}                        ← VAD 判停
服务端: conversation.item.input_audio_transcription.delta   {delta:"<用户文本片段>"}
服务端: response.output_text.delta       {delta:"<回复文本片段>"}
服务端: response.output_audio.started    {tts_type:"default|chat_tts_text|audit_content_risky|network"}
服务端: response.output_audio.delta      {delta:"<Base64 PCM24k>"}  ×N
服务端: response.output_audio.done       {status_code:"<0正常|20000002退出意图>"}
服务端: response.done                    {usage:{...}}            ← 本轮结束
```

**打断序列**：客户端 `response.cancel` → 服务端 `response.canceled`（ack）。
**清空会话**：客户端 `session.close` → 服务端 `session.closed` → 重新 `session.create`。
**重连续接**：`session.create` 携带上次 `session.id` 作为 `session.id` 字段；服务端拒绝/超时则新建。

**已知状态码/错误**：`20000002` 退出意图；`40000010` 空上下文删除；ASR 失败 `code:"transcription_error"/"audio_unintelligible"`。**完整错误码表在所给 PDF 之外的"接入必读"文档，待补充后细化 §7。**

## 5. UI 与触摸交互

### 5.1 主界面：对话气泡列表

- 屏幕：原生 410×502 竖屏，**UI 按横屏桌面布局设计，用 LVGL display rotation 转 90° 呈现**（M1 实测后确认旋转方向与触摸坐标映射）。
- 布局：顶部状态条 + 中部可滚动气泡区 + 底部提示文字。
- 气泡：用户右对齐（深色底）、机器人左对齐（浅色底），lv_obj 动态创建进 scroll 容器，流式追加文本，自动滚到底部；思考中显示省略号动画气泡。
- 状态胶囊：待机/聆听/思考/播报/重连中/错误 + WiFi 信号 + 音量图标。
- 本轮历史可滚动回看；notes_manager SD 落盘，重启后最近一轮恢复显示。
- **气泡数量上限 50 条**，超出删除最旧对象（防止动态创建/删除造成内存碎片；2 寸屏不做列表虚拟化）。

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

设置页加 API Key / 音色 voice / 语速 speed / 音量 loudness / 系统提示词输入项（settings NVS，secret 打码 + 显示/隐藏切换，网页顶部提示"局域网 HTTP 页面，仅建议在可信网络中使用"）；配网流程不变（AP captive portal）。

## 6. 对话数据流与状态机边界

### 6.1 一次对话数据流（含打断）

```
待机(唤醒词监听+屏保)
  │ 唤醒词 / 单击（退避等待中则取消退避立即重连）
  ▼
唤醒：wake_word_pause() → 确保 WSS 已连接 → 进入聆听态
  ├─ 采集任务 40ms/帧 → doubao_send_audio
  ├─ transcription.delta → 气泡实时显示用户说话
  ├─ 本地 VAD 静音1.5s 或 15s上限 → doubao_commit_audio()
  ▼
模型处理：output_text.delta 流式 → 机器人气泡逐字刷新
  ▼
播报：output_audio.delta → 3:2 重采样 → 环形缓冲 → 扬声器
  ├─ 播报中用户开口：本地能量检测(§3.3) → 停播/压低 + response.cancel → 回聆听态
  │   （服务端 transcription.started 到达后确认）
  ├─ response.done → 本轮结束
  │     ├─ 自动续听(开)：保持聆听，本地 VAD 空闲 8s（idle_timeout 可配）→ 回待机
  │     └─ 退出意图(20000002)：回待机
  ▼
待机：wake_word_resume() → 屏保倒计时
```

### 6.2 状态图

```
                ┌─────────── 单击/唤醒词/按键 ───────────┐
                ▼                                        │
   ┌───────┐   唤醒    ┌──────────┐   commit    ┌──────────┐
   │ 待机  │ ────────> │  聆听    │ ──────────> │  思考/   │
   │(唤醒词│           │ (上行帧) │             │ 流式文本 │
   │ +屏保)│ <──────── │          │             └────┬─────┘
   └───┬───┘  空闲8s   └────┬─────┘                  │ 音频delta
       │        ▲   本地VAD │        ┌───────────────┘
       │        │ 打断/续听  ▼        ▼
       │        │        ┌──────────┐
       │        └────────│   播报    │──双击──┐
       │   单击(打断进聆听)└──────────┘        ▼
       │                              ┌──────────┐
       │──长按──┐                     │ 停止播报  │
       │        ▼                     │ (回待机)  │
       │   ┌──────────┐  退出        └──────────┘
       └───│ 轻设置页  │───> 待机（不恢复对话）
           └──────────┘
   任意状态──断线──> 重连中（退避/立即重试，UI提示）
   任意状态──清空对话──> interrupt → session.close → 重建 → 待机
```

### 6.3 状态边界条件定义

| 边界 | 定义 |
|---|---|
| 清空对话时正在播报/聆听 | 先 `interrupt()` 停本地音频与响应 → `session.close` → 重建会话 → 清 UI 气泡 + SD 记录，回到待机 |
| 长按进设置页时正在对话 | 强制打断（停播 + 停上行 + response.cancel），退出设置页回**待机**（不自动恢复对话） |
| 自动续听"空闲 8s" | 以**本地 VAD** 为准（无语音），且无进行中的服务端响应 |
| 单击（播报中） | 打断并进入聆听 |
| 双击 | 停止播报（response.cancel + 停本地播放）并回**待机**（不进入聆听） |
| MP3 播放与对话冲突 | 沿用 Ver2.0 音频输出仲裁（MP3 播放时唤醒 → 停 MP3 进对话；对话中无 MP3 入口） |

## 7. 错误处理

| 场景 | 处理 |
|---|---|
| WSS 断线 | 指数退避 2→60s 重连；UI"重连中"；重连带 session.id 续接，失败则新建会话并提示 |
| API 错误事件 | error_log + UI 气泡提示；鉴权失败→提示检查 API Key |
| 限流/冷却 | 冷却 5s 自动恢复，UI 提示"稍后重试" |
| 音频设备异常 | codec 读写错误 → 重初始化音频子系统，不崩溃 |
| 内存保护 | 状态超时看门狗（思考30s/播报60s 强制回收）+ 任务看门狗 + mem_monitor |
| API 超时 | **按状态细分**：commit 后 15s 未收到任何 output 事件 → 超时重连；播报中 5s 无 audio delta → 超时重连。**聆听态不设下行超时**（用户静音时服务端本就无下行事件），连接健康以 WS ping/pong + 上行发送成功判定 |

## 8. 测试

- 集成测试清单（照搬 PRD integration-test 方法）：启动序列日志验收（Board→I2C→codec→WiFi→WSS→Ready）→ 单轮对话 → 打断 → 连续多轮 → 清空对话 → 断网重连 → 唤醒词。
- **全双工/回声专项**：
  1. 播报中用户开口 → 本地快速停播（<100ms）、服务端未被自身播报声误导、用户语音识别准确；
  2. 高音量播报中无人说话 → 本地 VAD/能量检测不误触发打断；
  3. 长回复播报中连续多次打断 → 状态机稳定无卡死；
  4. 2 小时全双工运行 + mem_monitor 内存曲线 + 各任务栈高水位监控（无泄漏、无碎片失控）。
- PC 音频模拟（PRD audio-emulation 方法）做自动化语音回放测试。
- 串口 CLI：`talk`（触发对话）、`doubao status`（会话/连接状态）、`say <文本>`（speech_text_buffer 直推）、`reboot`。
- 稳定性：2 小时连续运行 + mem_monitor 内存曲线（无泄漏无崩溃）。

## 9. 里程碑

| 阶段 | 内容 | 验收 |
|---|---|---|
| M1 | 骨架移植：复制裁剪编译烧录，board+UI+音频验证 | 屏幕点亮、播放测试音、mic 采到音；**旋转 90° 显示 + 触摸坐标映射验证；同时播放+录音回路测试（ES8311/ES7210 共享时钟下双工稳定）** |
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
| FR-07 打断 | 本地能量检测（一级）+ transcription.started（二级）+ response.cancel（§3.3、§4.3、§6） |
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

1. 豆包完整错误码表不在所给 PDF 内——v1 通用错误处理，遇到具体错误码再细化（§4.4）。
2. Opus 编解码（上行 speech_opus / 下行 ogg_opus）可省带宽，v2 优化项。
3. 全双工期间 ES8311 播放 + ES7210 采集同时工作的稳定性**提前到 M1 回路测试验证**（Ver2.0 未验证过该组合）。
4. 回声风险分层控制（§3.2）：播放感知门控（v1）→ 服务端鲁棒性实测 → esp-sr AEC（后备）。
5. 3:2 多相重采样 CPU 开销未实测——M2 评估，超出预算降级带限线性插值。
6. API Key 凭据安全：v1 = NVS 存储 + 网页输入（无明文硬编码）+ UI 打码/显示切换 + 网页"仅可信局域网"提示；NVS 加密/Flash 加密为 v2 加固项（威胁模型：桌面设备物理读取场景低，加密增加 bootloader 复杂度）。
7. 网页为局域网 HTTP——v1 仅提示可信网络，自签名 HTTPS 暂不做（LAN 场景收益低）。

## 12. 修订记录（真机联调后，2026-08-21）

依据真机日志 + 官方 Python demo 对照（demo/python3.7_duplex_demo/）修订，覆盖 §3、§6、§7：

1. **播报期上行保持（静音泵）**——修订 §3.4/§6.1。时序分段模型下播报期采集暂停，send 任务必须以 20ms/320 样本的真实零值 PCM 帧维持上行流。实测：上行断流 → 服务端 TTS 停摆 → ~30s 后 `55000000/52000016 AudioTTSIdleTimeoutError` 杀会话（demo 的 mic 线程全程不停，流从不中断）。"静音=采集帧清零"的旧假设不成立（采集停了就没有帧源）。
2. **状态转移修正**——修订 §6.1/§6.2。THINKING 边 = `transcription.completed`（旧实现第一条 delta 就进 THINKING，用户还在说话）；COMMITTING = commit 实际发出后（VAD 回调置 `s_commit_pending`，tick 晋升），15s 看门狗从该边起算，transcript delta 重置计数（长句不误触发）；transcript 流式期间保持 LISTENING。
3. **会话生命周期**——修订 §7。服务端主动 `session.closed`（错误/空闲超时）→ `dbws_session_mark_closed()` 清除全部会话标志（旧实现残留 active，下次唤醒不重建会话 → 上行全被无视）；chat 的 SESSION_CLOSED/ERROR 处理器在非 IDLE 态执行 `go_idle()`（错误不再卡 UI ERROR，唤醒词回来用户直接重试——与 demo error→结束会话→可重开一致）。
4. **唤醒词恢复**——修订 §6.1。`go_idle()` 显式 `wake_word_resume()`：`app_set_state(UI_STATE_IDLE)` 绕过旧状态机 `on_enter_state(IDLE)` 钩子，旧实现对话结束后唤醒词永不恢复（设备聋，真机确认）。
5. **RX 抗丢帧**——修订 §4.3。rx ring 8→32 槽；按 `payload_offset==0` 的消息边界复位片段累积（掉帧只损失所在消息，不再毒化后续消息导致 64KB 溢出）；掉帧计数变化即复位；dbws 任务 RX 处理优先于 TX 排空，send 锁等待 200→50ms。
6. **开机 TTS 播报**——speech_text 必须有会话，开机无会话 → 跳过并降级 INFO（不再喂 consecutive-error 计数器）。
7. **播报缓冲与收尾**——修订 §3.4/§6.2（2026-08-21 第二次真机日志：断断续续）。服务端以 ~2× 实时速度突发推送 TTS（4.8s 音频 2.3s 送达），旧 2s 播放环每次回复必溢出、丢最旧样本 → 语音被跳切。修复：播放环 64KB→1MB（32.8s）；audio.done 不再即时停播，改为 `dbaudio_out_drain_stop()` 排空积压（SPEAKING 持续到环干，tick 再转 auto-listen，回复尾巴不再被切）；打断（interrupt）改为丢弃积压而非排空（环变大后排空会让"停止"再播 30s）。
8. **音质/显示/交互打磨**（2026-08-21 第三次真机日志：轻微刺啦声 + 用户需求）：
   - **刺啦声**：播放首尾硬阶跃（静音预滚→全幅音频、音频→静音，`apply_fade` 一直未被调用）→ 启用首 50ms 淡入 + drain 收尾淡出；session.create 的 dialog.extra 加 `enable_loudness_norm:true`（demo 同款，防 TTS 削波）；`CONFIG_I2S_ISR_IRAM_SAFE=y`（铁律 9）。
   - **竖屏**：移除 `bsp_display_rotate(ROTATION_90)`，原生竖屏 410×502（同 Ver2.0；UI 本就按竖屏尺寸编写）。
   - **字体**：所有承载中文的 label 统一 `SourceHanSansCN_Medium_16`（气泡、中文标题、mp3 曲名/歌手、状态消息、设置页返回键）。
   - **提示音与按键**（Ver2.0 同款）：进入聆听播 `snd_rec_start`（叮咚）；LISTENING/THINKING 单击 BOOT → `doubao_chat_cancel()`（go_idle + `snd_rec_stop` 咚叮，唤醒词恢复）。
   - **日志精简**：VAD 逐帧 silence、proto 下行事件+raw、dispatch、chat_event、tick 等诊断日志降为 ESP_LOGD（LOG_MAXIMUM=INFO 下编译剔除）；TX 队列 16→32 槽。
9. **第四轮真机日志优化**（2026-08-21 21:55 日志）：
   - **SPI DMA 分配失败**（`setup_dma_priv_buffer: Failed to allocate priv TX buffer`，播报期屏/SD 卡顿风险）：根因 = cJSON 解析在内部堆大量小节点分配（ALWAYSINTERNAL=256 下 <256B 全落内部），2× 音频突发期间挤爆内部 RAM。修复：protocol.c 里 `cJSON_InitHooks` 全局钩子改用 PSRAM malloc/free。
   - **TX 饿死**（"Could not lock ws-client" + "TX queue full" 洪水）：根因 = ws 客户端内部任务在连续 RX 突发期间 give→poll→take 循环无让渡点，同优先级（5）的 dbws 任务抢不到锁发送。修复：dbws 任务优先级 5→7（ws 内部任务保持 5），锁一释放即可发送；仍低于采集 9/播放 10（铁律 7）。
   - **气泡样式**（用户需求）：聊天气泡统一黑底（0x000000）+ 白字 + 细灰边框（0x333333）；用户 STT 气泡居右、文本右对齐，AI 回复气泡居左、文本左对齐；气泡列表仍在屏幕底部（y=380 起，底对齐滚动）。
10. **第五轮：卡顿与气泡样式修订**（2026-08-21 22:17 日志）：
    - **AI 播报轻微卡顿 / 叮咚咚叮卡顿**：播放环健康（play loop 显示 ring 13058/6157 样本、无下溢）→ 卡顿在 I2S TX DMA 层。旧 DMA 6×240 帧 @16kHz = 90ms 深度（原按 48kHz 调优），偶发停滞即断流。修复：dma_desc_num 6→10（150ms）；提示音改经 audio_out 播放任务（prio 10）播放（`dbaudio_out_play_tone()`，静音垫吃掉淡入斜坡保证音头清脆）——低优先级任务直写是叮咚卡顿主因；play 任务对 codec 写失败加限速告警（可观测）。
    - **TX 争用收尾**：drain 仅在 rx ring 清空时执行（突发期不白耗锁等待）；send 锁等待 50→250ms（软件 AES 解 22KB 消息需 100-500ms，长等才能推进）；入队超时 50→10ms（队满时泵节奏快速恢复）；`esp_log_level_set("websocket_client", WARN)` 消除良性锁争用 E 刷屏（真实断连仍由自研 handler 记录）。残余认识：库设计上锁横跨整条消息接收，TX 在持续 RX 突发期必然排队——丢的只是静音泵帧，对语音与协议无害。
    - **气泡样式修订**（用户需求）：无边框、透明背景（与黑屏融为一体）、文本水平居中；用户 STT 用稍暗白 0xBBBBBB、AI 回复纯白 0xFFFFFF 以区分双方。
11. **第六轮：播报平滑性**（2026-08-21 22:37 日志：播报仍有卡顿/丢帧感）：
    - 日志量测：drain 期播放速率 ≈1.03×（完美），交付期只消费了 ~0.78×——交付期存在 ~0.84s 的有效停滞；codec 写零失败（排除 codec/DMA 喂失败）。
    - 修复：①欠载补丁路径不再补零——改播实际样本（补零曾在每次交付间隙插入 20-40ms 静音丢字，即"丢帧感"）；②首播前预缓冲 0.5s（PREBUF_SAMPLES=8192，提示音经 s_skip_prebuf 跳过）；③可观测性：play 任务记录"loop 慢"（>60ms 循环周期，限速）与"ring empty"（真实输出缺口，限速）——下一轮日志可直接定位残余停滞机制；④静音泵自适应节流：TX 队列积压 >8 帧时跳过发送（消除 TX queue full 丢帧与泵节奏紊乱，流仍存活）。
12. **第七轮：DMA 池耗尽与状态机卡死**（2026-08-21 用户贴日志）：
    - **`setup_dma_priv_buffer: Failed to allocate priv TX buffer` 洪水**：根因 = I2S DMA 10 描述符（9.6KB）吃掉 DMA 池，SD 卡 SPI 事务（notes 落盘）在 DMA 池只剩 ~3.4KB 时分配不到私有 DMA 缓冲 → 快速重试刷屏 → UART/任务整体失速 → rx ring 溢出（124 掉片）+ TX queue full 连带洪水。修复：**I2S DMA 回退 6 描述符（90ms）**——150ms 深度对平滑性无贡献（上轮量测 drain 1.03×/交付期停滞不在 DMA），但 9.6KB 的 DMA 代价是系统性的。
    - **`state_machine: Rejected transition: 7 -> 5`（状态机卡死）**：根因 = doubao 路径在进入 LISTENING 之后全部用 `app_set_state` 旁路旧状态机，旧机器 `s_current_state` 停留在 THINKING(7)；唤醒词/单击/触摸再请求 `app_state_request(LISTENING)` 时被转移表拒绝（THINKING→LISTENING 非法）→ 唤醒词打断/下一轮直接失效。修复：app_main 三处进入 LISTENING 的请求全部改 `app_set_state`（旧机器只保留 CONNECTING/IDLE 等骨架态；唤醒词暂停由 start_listening 显式负责）。

## 13. 与 res.prd 架构对照（2026-08-22）

对照 `/home/conor/esp/esp32/AIWatch_Doubao/res.prd`（豆包 Seeduplex 架构建议），采纳其可落地原则，逐条修复当前日志问题：

| res.prd 原则 | 现状 | 处理 |
|---|---|---|
| 并行子状态机（连接/会话/采集/对话/播放分开建模） | 已按此分层（ws_client 会话标志、chat 状态、audio_out 播放态），但 UI 层残留双头状态机（7→5 拒绝） | 上一轮已修复入口请求；不再重写 |
| 事件队列通信、WS 任务不做重活 | WS 内部任务只拷贝片段 ✓；dbws 任务做解析/重采样 | 保持 |
| **播放中随时插话 + generation 丢弃旧音频**（§8） | 打断清空播放环，但**迟到旧 delta 无门控** | 新增 `s_audio_stream_open` 门（AUDIO_STARTED 开、AUDIO_DONE/本地 interrupt 关），迟到音频丢弃 |
| 发送不得阻塞接收 | drain 锁等待 250ms 曾阻塞 dbws 任务 → rx 溢出 → 音频丢失 → ring empty 76 次（本轮日志实锤卡顿链） | drain 改 **try-lock（timeout 0）**，发送在锁空闲间隙推进；泵节流兜底 |
| 下行缓冲吸收突发 | rx ring 32 槽在快速 TTS flush 时溢出（掉片 40） | rx ring 64 槽（268KB PSRAM） |
| SPI/DMA 内存纪律 | SD SPI max_transfer_sz=65536 → 每事务 64KB 内部 DMA 私有缓冲 → 64KB 保留池必失败 → 507 条重试洪水 | max_transfer_sz → 4096（4KB 事务缓冲可分配） |
| 超时设计 | 已有（思考 30s/播报 60s/commit 15s/session.create 5s） | 保持 |
| API Key 不入固件 | 走 NVS + 网页（§5.6）✓ | 保持 |
| WS 事件回调必须轻（铁律 8/22） | ❌ CONNECTED/DISCONNECTED 回调内联调用 s_cb → chat go_idle 在 ws 内部任务退出路径里做 400ms 阻塞停等 → 任务在 destroy 后仍存活，与库的延迟释放机制 double-free（真机 3 连崩：LoadProhibited / xEventGroupSetBits assert / LoadStoreAlignment，全在 esp_websocket_client_task） | 修复：handler 只置 pending 标志 + 事件位，dbws 任务分发事件；create_client 增加陈旧句柄清理 |
| 库自身销毁竞态（第二轮崩溃根因） | esp_websocket_client 1.1.0 上游 bug：传输错误 abort 路径置 run=false + selected_for_destroying，destroy() 见 run==false 跳过 join 立即释放 → 任务收尾阶段（transport_close/STOPPED_BIT/state 写/自释放）与释放竞态 double-free | **本地化覆盖 components/esp_websocket_client**：destroy() 恒 join（run==false 且有任务时等 STOPPED_BIT）；任务退出路径 STOPPED_BIT 设为最后访问、任务不自释放（destroy 为唯一释放点）。离线构建配套：托管目录从组件管理器缓存恢复 + 补 .component_hash |

