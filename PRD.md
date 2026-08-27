# 豆包 AI 桌面对话机器人 — 产品需求文档 (PRD)

| 字段 | 内容 |
|------|------|
| 产品名称 | Doubao Voice Robot |
| 版本 | 0.1.0 (DVT — 设计验证测试阶段) |
| 日期 | 2026-08-27 |
| 硬件平台 | Waveshare ESP32-S3-Touch-AMOLED-2.06 |
| 云端服务 | 火山引擎 豆包语音·端到端实时语音（全双工版本） |
| 模型 | Seeduplex `model = 1.2.6.1` |

---

## 一、产品概述

### 1.1 产品定位

一款基于 ESP32-S3 的桌面 AI 语音对话机器人，通过单条 WebSocket 全双工连接与豆包实时语音 API 通信，实现"开口即说、随时打断"的自然对话体验。产品形态为桌面摆件，配备 AMOLED 触摸屏，支持语音唤醒、触摸交互和按键操作。

### 1.2 目标用户

- 桌面办公场景的 AI 助手用户
- 需要语音交互的智能家居控制场景
- 开发者和 IoT 爱好者（开源项目）

### 1.3 核心价值

| 特性 | 价值 |
|------|------|
| 全双工对话 | 播报中可随时打断，无需等待说完 |
| 本地唤醒词 | 离线检测"你好小智"，无需联网 |
| 多模态交互 | 语音 + 触摸 + 按键，适应不同场景 |
| 设备控制 | 语音控制音量/亮度/音乐等，AI 可主动操控设备 |
| 连续对话 | 自动续听，支持多轮上下文 |

---

## 二、硬件规格

### 2.1 主控板（Waveshare ESP32-S3-Touch-AMOLED-2.06）

| 组件 | 规格 | 说明 |
|------|------|------|
| MCU | ESP32-S3R8 | 双核 Xtensa LX7 @ 240MHz |
| Flash | 32MB | QSPI, 80MHz, QIO 模式 |
| PSRAM | 8MB | Octal SPI, 80MHz |
| 显示屏 | 410×502 AMOLED | CO5300/SH8601 驱动, QSPI, 16-bit RGB565 |
| 触摸 | FT3168 电容式 | I2C 地址 0x38 |
| 音频 DAC | ES8311 | 扬声器输出, I2C 地址 0x18 |
| 音频 ADC | ES7210 | 四通道麦克风输入, I2C 地址 0x40 |
| IMU | QMI8658 | 6 轴（加速度计+陀螺仪） |
| PMU | AXP2101 | 电源管理 |
| RTC | PCF85063 | 实时时钟 |
| SD 卡 | MicroSD via SPI | SPI3_HOST（与显示 QSPI 分离） |
| 按键 | BOOT (GPIO0) + PWR (GPIO10) | BOOT 低电平有效 |

### 2.2 音频链路

```
麦克风 ES7210 ──I2S RX 16k/16bit──> 采集任务 ──640样本(40ms)/帧──> Base64 ──> input_audio_buffer.append

扬声器 ES8311 <──I2S TX 16k/16bit── 播放任务 <──3:2多相重采样24k→16k── 播放环形缓冲(PSRAM,1MB) <── output_audio.delta(Base64)
```

| 参数 | 值 |
|------|-----|
| I2S 总线 | 单总线（I2S1），TX/RX 共享时钟 |
| 采样率 | 16kHz（硬件固定） |
| 帧大小 | 640 样本 = 40ms |
| 上行带宽 | ≈42KB/s PCM |
| 下行带宽 | ≈64KB/s Base64 |
| 播放环形缓冲 | 1MB PSRAM（32.8 秒容量） |
| DMA 描述符 | 6 个（90ms 深度） |

### 2.3 支持的硬件变体

通过 Kconfig 菜单选择：

| 变体 | 显示屏 | RGB 环 | 按键 |
|------|--------|--------|------|
| Waveshare ESP32-S3-Touch-AMOLED-2.06 | 410×502 AMOLED | 无 | BOOT |
| Waveshare ESP32-S3-AUDIO-Board | 1.47" IPS LCD | 7× WS2812 | BOOT + BTN1 |
| M5StickC Plus2 | 1.14" IPS LCD | 无 | A/B/C 三键 |
| SenseCAP Watcher | SPD2010 | 无 | 按键 |

---

## 三、软件功能需求

### 3.1 功能需求列表

| 编号 | 功能 | 优先级 | 状态 |
|------|------|--------|------|
| FR-01 | 语音唤醒 | P0 | ✅ 已实现 |
| FR-02 | 麦克风采集（16k/16bit/mono） | P0 | ✅ 已实现 |
| FR-03 | 本地 VAD（静音检测） | P0 | ✅ 已实现 |
| FR-04 | 豆包 WSS 全双工对话 | P0 | ✅ 已实现 |
| FR-05 | 流式文本回复 | P0 | ✅ 已实现 |
| FR-06 | 语音播报（TTS） | P0 | ✅ 已实现 |
| FR-07 | 本地打断（播报中可打断） | P0 | ✅ 已实现 |
| FR-08 | 多轮上下文（20 轮） | P1 | ✅ 已实现 |
| FR-09 | 对话气泡列表 | P1 | ✅ 已实现 |
| FR-10 | 状态指示（颜色编码） | P1 | ✅ 已实现 |
| FR-11 | 流式文本显示 | P1 | ✅ 已实现 |
| FR-12 | 触摸手势交互 | P1 | ✅ 已实现 |
| FR-13 | 轻设置页 | P2 | ✅ 已实现 |
| FR-14 | 屏保（防烧屏） | P2 | ✅ 已实现 |
| FR-15 | WiFi 配网（AP Captive Portal） | P1 | ✅ 已实现 |
| FR-16 | 断线自动重连 | P1 | ✅ 已实现 |
| FR-17 | API Key NVS 存储 | P1 | ✅ 已实现 |
| FR-18 | 系统提示词配置 | P2 | ✅ 已实现 |
| FR-19 | 上下文裁剪 | P2 | ✅ 已实现（服务端托管） |
| FR-20 | 清空对话 | P2 | ✅ 已实现 |
| FR-21 | 内容安全过滤 | P2 | ✅ 已实现 |
| FR-22 | 语音控制设备（[DEVICE:] 指令） | P1 | ✅ 已实现 |
| FR-23 | 异常恢复（超时/错误/内存保护） | P0 | ✅ 已实现 |
| FR-24 | OTA 升级 | P3 | 🔲 预留接口 |
| FR-25 | 串口 CLI 调试 | P2 | ✅ 已实现 |

### 3.2 功能详细说明

#### FR-01 语音唤醒

- **唤醒词**："你好小智"（ESP-SR WakeNet，离线检测）
- **触发方式**：唤醒词 / 触摸单击 / BOOT 按键
- **唤醒反馈**：播放"叮咚"提示音（`snd_rec_start`，190ms）
- **仲裁机制**：待机时 WakeNet 独占麦克风；对话期 `wake_word_pause()`；对话结束 `wake_word_resume()`

#### FR-03 本地 VAD

- **算法**：自适应噪声底（移植自 AIWatch_Ver2.0）
- **静音阈值**：默认 1.5 秒（可配置 `silence_timeout_ms`）
- **最长录音**：15 秒（可配置 `max_record_seconds`）
- **判停后**：发送 `input_audio_buffer.commit` 给服务端

#### FR-07 本地打断

- **检测方式**：能量检测（用户声音 > 播放声音 + 6dB 余量）
- **持续时间**：200ms（满足 80ms 要求）
- **打断动作**：立即本地停播 + 发送 `response.cancel`
- **服务端确认**：`transcription.started` 到达后确认状态

#### FR-22 语音控制设备

AI 可通过 `[DEVICE:command]` 标签控制设备：

| 命令 | 说明 |
|------|------|
| `[DEVICE:volume=0-100]` | 调节音量 |
| `[DEVICE:brightness=0-100]` | 调节亮度 |
| `[DEVICE:mp3=stop]` | 停止音乐 |
| `[DEVICE:mp3=pause]` / `[DEVICE:mp3=resume]` | 暂停/恢复 |
| `[DEVICE:mp3=show]` | 显示曲目列表 |
| `[DEVICE:mp3=index:N]` | 按序号播放 |
| `[DEVICE:mp3=play:<file>]` | 按文件名播放 |
| `[DEVICE:rgb=rainbow/aurora/fire/ocean/off/on]` | RGB 灯效控制 |
| `[DEVICE:sleep=N]` | 设置休眠超时（分钟） |
| `[DEVICE:reboot]` | 重启设备 |
| `[DEVICE:webserver=on/off]` | 网页服务器开关 |
| `[DEVICE:auto_read=on/off]` | 自动朗读开关 |

---

## 四、系统工作流

### 4.1 启动工作流

```
app_main()
  │
  ├─ 1. settings_init()                    // NVS 配置加载
  ├─ 2. cJSON_InitHooks(PSRAM)             // cJSON 用 PSRAM 分配
  ├─ 3. error_log_init()                   // 错误日志
  ├─ 4. xEventGroupCreate()               // 全局事件组
  ├─ 5. app_state_machine_init()           // 状态机初始化
  ├─ 6. board_init()                       // 硬件初始化 (I2C, codec, display)
  ├─ 7. board_audio_set_volume()           // 音量设置
  ├─ 8. board_display_set_brightness()     // 亮度设置
  ├─ 9. board_sdcard_init()                // SD 卡初始化
  ├─ 10. mp3_player_init()                 // MP3 播放器初始化
  ├─ 11. ui_init()                         // LVGL UI 初始化
  ├─ 12. app_set_state(UI_STATE_BOOT)      // 显示启动画面
  ├─ 13. wifi_manager_init()               // WiFi 连接
  ├─ 14. xEventGroupWaitBits(WIFI)         // 等待 WiFi (30s 超时)
  ├─ 15. [可选] webserver_start()           // 网页服务器
  ├─ 16. SNTP 时间同步                      // 等待时间
  ├─ 17. doubao_init_from_settings()       // 豆包 WSS 初始化
  ├─ 18. wake_word_init()                  // 唤醒词引擎
  ├─ 19. app_sd_mp3_scan_init()            // 扫描 SD 卡 MP3
  ├─ 20. notes_manager_init()              // 聊天记录管理
  ├─ 21. serial_cmd_task_start()           // 串口 CLI
  │
  └─ 22. 主事件循环 while(1)
```

### 4.2 典型对话工作流

```
┌─────────────────────────────────────────────────────────────────┐
│                      IDLE (待机态)                              │
│  - 唤醒词引擎运行中                                            │
│  - 屏幕显示时钟 + 日期                                         │
│  - RGB 绿色呼吸灯                                              │
└───────────────────────────┬─────────────────────────────────────┘
                            │ 唤醒词 / 单击 / BOOT 键
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      LISTENING (聆听态)                         │
│  1. wake_word_pause() — 暂停唤醒词                             │
│  2. doubao_ensure_session() — 创建会话                         │
│  3. dbaudio_in_start() — 开始采集                              │
│  4. 播放"叮咚"提示音                                           │
│  5. 40ms/帧上传音频                                            │
│  6. VAD 检测静音 → commit                                      │
└───────────────────────────┬─────────────────────────────────────┘
                            │ VAD 判停 / s_commit_pending=true
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      COMMITTING (提交态)                        │
│  doubao_commit_audio() → input_audio_buffer.commit             │
│  - 15s 看门狗启动                                              │
│  - transcript.delta 流式更新用户气泡                           │
└───────────────────────────┬─────────────────────────────────────┘
                            │ transcription.done
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      THINKING (思考态)                          │
│  - 等待 AI 回复                                                │
│  - output_text.delta → 气泡流式显示                            │
│  - 30s 看门狗                                                  │
└───────────────────────────┬─────────────────────────────────────┘
                            │ output_audio.started
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      SPEAKING (播报态)                          │
│  1. dbaudio_in_stop() — 暂停采集（避免回声）                   │
│  2. doubao_set_uplink_muted(true) — 发送静音帧保活             │
│  3. output_audio.delta → 重采样 24k→16k → 环形缓冲 → 扬声器   │
│  4. 打断检测: mic_rms > play_rms*2 持续 200ms                  │
│  5. 60s 看门狗                                                 │
└───────────────┬─────────────────────┬───────────────────────────┘
                │                     │
                │ audio.done          │ 本地打断
                ▼                     ▼
┌─────────────────────┐   ┌─────────────────────────────────────┐
│  drain 环形缓冲     │   │  interrupt + dbaudio_out_stop()     │
│  (不立即停播)       │   │  → 回到 LISTENING                   │
└─────────┬───────────┘   └─────────────────────────────────────┘
          │ ring 干涸
          ▼
┌─────────────────────────────────────────────────────────────────┐
│                      AUTO_LISTEN (自动续听态)                    │
│  1. dbaudio_in_start() — 恢复采集                               │
│  2. 等待用户下一句话                                           │
│  3. idle_timeout (默认 8s) 无语音 → 回 IDLE                    │
└───────────────────────────┬─────────────────────────────────────┘
                            │ 用户说话 / 超时
                            ▼
                      LISTENING 或 IDLE
```

### 4.3 事件处理工作流

主循环每 100ms 处理一次事件：

| 事件位 | 触发条件 | 处理逻辑 |
|--------|----------|----------|
| `WAKE_WORD_BIT` | 唤醒词检测到 | 停止冲突子系统 → 播放叮咚 → 进入 LISTENING → doubao_chat_start() |
| `KNOB_PRESSED_BIT` | BOOT 键短按 | IDLE/RESPONSE → 进入对话；TTS → 打断；LISTENING → 取消 |
| `TOUCH_BIT` | 触摸单击 | IDLE → 开始对话；RESPONSE → 关闭；播放中 → 取消 |
| `DOUBLE_TAP_BIT` | 触摸双击 | doubao_interrupt() → 回 IDLE |
| `SETTINGS_LONG_PRESS_BIT` | 长按 1s | 打断对话 → 显示设置页 |
| `MP3_CMD_BIT` | MP3 命令 | 执行 play/stop/pause/resume/show/scan |
| `DOUBAO_START_BIT` | 对话启动入口 | doubao_chat_start_listening() |
| `WEBSERVER_TOGGLE_BIT` | Web 服务器开关 | start/stop webserver |
| `CANCEL_BIT` | 取消录制 | doubao_chat_cancel() |

### 4.4 WiFi 连接工作流

```
wifi_manager_init()
  │
  ├─ 尝试 STA 模式连接 (ssid + password)
  │   ├─ 成功 → WIFI_CONNECTED_BIT → 继续启动
  │   └─ 失败 → 启动 AP 模式
  │       ├─ 开启 Captive Portal
  │       ├─ 启动 Web 服务器 (192.168.4.1)
  │       ├─ 显示 QR 码
  │       └─ 用户通过网页配置 WiFi + API Key
  │
  └─ WiFi 断连处理
      ├─ WIFI_EVENT_STA_DISCONNECTED
      ├─ 销毁 WS 句柄
      ├─ 退避重连 (2s → 60s 指数退避)
      └─ GOT_IP 后重建 WS
```

### 4.5 MP3 播放工作流

```
用户说"播放音乐" 或 [DEVICE:mp3=...]
  │
  ├─ AI 回复包含 [DEVICE:mp3=show]
  │   └─ 显示曲目列表 → 用户选择 → [DEVICE:mp3=index:N]
  │
  ├─ AI 回复包含 [DEVICE:mp3=index:N]
  │   ├─ mp3_player_play(filename)
  │   ├─ app_set_state(UI_STATE_PLAYING_MP3)
  │   ├─ 显示 MP3 UI 覆盖层
  │   └─ 播放完成 → on_mp3_complete() → app_queue_mp3_cmd("stop")
  │
  └─ 用户说"停止音乐" 或 [DEVICE:mp3=stop]
      ├─ mp3_player_stop()
      ├─ ui_mp3_ui_hide()
      └─ app_set_state(UI_STATE_IDLE)
```

---

## 五、状态机机制

### 5.1 双层状态机架构

本系统采用**双层状态机**设计：

| 层级 | 文件 | 职责 |
|------|------|------|
| **Legacy 状态机** | `app_state_machine.c` | 资源仲裁（I2S TX/RX 所有权）、转移合法性校验 |
| **Doubao 对话状态机** | `doubao_chat.c` | 对话流程编排（VAD、提交、打断、自动续听） |

### 5.2 Legacy 状态机

#### 5.2.1 状态枚举

```c
typedef enum {
    UI_STATE_SLEEP = 0,       // 深度睡眠
    UI_STATE_ARMED,           // 唤醒词待命
    UI_STATE_BOOT,            // 启动中
    UI_STATE_CONNECTING,      // WiFi/WS 连接中
    UI_STATE_IDLE,            // 待机
    UI_STATE_LISTENING,       // 聆听中
    UI_STATE_SENDING,         // 上行发送中
    UI_STATE_THINKING,        // AI 思考中
    UI_STATE_STREAMING,       // 流式文本接收中
    UI_STATE_RESPONSE,        // 回复展示
    UI_STATE_TTS_LOADING,     // 音频加载中
    UI_STATE_TTS_PLAYING,     // 语音播报中
    UI_STATE_PLAYING_MP3,     // SD 卡 MP3 播放
    UI_STATE_NOTIFYING,       // 通知播报
    UI_STATE_ERROR,           // 错误
} ui_state_t;
```

#### 5.2.2 合法转移表

```
BOOT       → CONNECTING, IDLE
CONNECTING → IDLE, ERROR
IDLE       → LISTENING, PLAYING_MP3, NOTIFYING, CONNECTING, ERROR
LISTENING  → SENDING, THINKING, IDLE, ERROR
SENDING    → THINKING, IDLE, ERROR
THINKING   → STREAMING, IDLE, LISTENING, ERROR
STREAMING  → RESPONSE, IDLE, ERROR
RESPONSE   → TTS_LOADING, TTS_PLAYING, IDLE, ERROR
TTS_LOADING→ TTS_PLAYING, IDLE, ERROR
TTS_PLAYING→ IDLE, LISTENING, ERROR
PLAYING_MP3→ IDLE, ERROR
NOTIFYING  → IDLE, ERROR
ERROR      → CONNECTING, IDLE
```

#### 5.2.3 资源仲裁

**资源类型**：
- `RES_AUDIO_OUT` (I2S TX)：扬声器输出
- `RES_AUDIO_IN` (I2S RX)：麦克风输入

**各状态所需资源**：
```
LISTENING  → {RES_AUDIO_IN, RES_AUDIO_OUT}  // 全双工
TTS_PLAYING→ {RES_AUDIO_OUT}
PLAYING_MP3→ {RES_AUDIO_OUT}
NOTIFYING  → {RES_AUDIO_OUT}
其他状态   → 无独占资源
```

**优先级（数值越小优先级越高）**：
```
LISTENING   = 1  // 最高
PLAYING_MP3 = 2
TTS_PLAYING = 3
NOTIFYING   = 4
其他        = 9  // 最低
```

**抢占规则**：高优先级状态可抢占低优先级状态的资源。例如，LISTENING 可抢占 TTS_PLAYING 的 I2S TX 资源。

#### 5.2.4 核心函数 app_state_request() 流程

```
app_state_request(target)
  │
  ├─ 1. 加互斥锁
  ├─ 2. 校验转移合法性 (查表)
  ├─ 3. 若目标需要资源且被占用:
  │     ├─ 目标优先级 > 当前持有者 → 抢占 (强制 IDLE)
  │     └─ 目标优先级 ≤ 当前持有者 → 拒绝
  ├─ 4. 释放旧状态资源
  ├─ 5. 获取新状态资源
  ├─ 6. 更新 s_current_state
  ├─ 7. 释放互斥锁
  ├─ 8. 执行 Hook (在锁外):
  │     ├─ on_leave_state: wake_word_resume()
  │     └─ on_enter_state: wake_word_pause()
  └─ 9. 应用 UI 状态 app_set_state()
```

#### 5.2.5 线程安全机制

- **互斥锁**：`s_state_mutex` 保护 `s_current_state` 和 `s_resource_owner[]`
- **Hook 外置**：`wake_word_pause/resume()` 在互斥锁外执行（可能阻塞 500ms）
- **强制清理**：`app_state_force_idle()` 不递归获取锁（避免死锁）
- **S4 修复**：仅在状态仍为当前状态时才更新为 IDLE

### 5.3 Doubao 对话状态机

#### 5.3.1 对话状态枚举

```c
typedef enum {
    CHAT_IDLE,          // 等待启动
    CHAT_LISTENING,     // 麦克风采集，VAD 活跃
    CHAT_COMMITTING,    // commit 已发送，等待服务端响应
    CHAT_THINKING,      // 服务端处理中（已收到转录）
    CHAT_SPEAKING,      // 音频 delta 播放中
    CHAT_AUTO_LISTEN,   // 响应完成，自动续听
} chat_state_t;
```

#### 5.3.2 状态转移图

```
                    ┌────── 单击/唤醒词/按键 ──────┐
                    ▼                              │
         ┌──────────────┐    commit    ┌──────────┐
    ┌────│    IDLE      │ ──────────> │LISTENING │
    │    │ (唤醒词监听) │ <────────── └────┬─────┘
    │    └──────────────┘     空闲8s       │ VAD
    │                                      ▼
    │                              ┌──────────────┐
    │                              │  COMMITTING  │
    │                              │ (等待响应)   │
    │                              └──────┬───────┘
    │                                     │ transcript.done
    │                                     ▼
    │                             ┌──────────────┐
    │                             │  THINKING    │
    │                             │ (流式文本)   │
    │                             └──────┬───────┘
    │                                    │ audio.started
    │                                    ▼
    │    ┌──────────────┐         ┌──────────────┐
    └───>│ AUTO_LISTEN  │<────────│  SPEAKING    │
         │ (等待续听)   │ audio.done│ (播报中)    │
         └──────────────┘ drain   └──────┬───────┘
                                         │ 打断
                                         └────────> LISTENING
```

#### 5.3.3 超时参数

| 参数 | 值 | 说明 |
|------|-----|------|
| `THINKING_TIMEOUT_S` | 30s | 思考态无 output → 错误 |
| `COMMIT_TIMEOUT_S` | 15s | commit 后无 output → 错误 |
| `SPEAK_TIMEOUT_S` | 60s | 播报中无 audio delta → 停止 |
| `INTERRUPT_DB_MARGIN` | 6.0f | 打断需要用户声音 > 播放声音 + 6dB |
| `INTERRUPT_HOLD_MS` | 80ms | 打断需要持续 80ms |
| `session.create 超时` | 5s | 会话创建超时 → 回 IDLE |
| `RESPONSE_TIMEOUT_MS` | 60s | 回复展示超时 → 回 IDLE |

#### 5.3.4 关键回调事件

| 事件 | 来源 | 处理 |
|------|------|------|
| `DOUBAO_EVT_CONNECTED` | WSS 连接成功 | CONNECTING/ERROR → IDLE |
| `DOUBAO_EVT_DISCONNECTED` | WSS 断开 | 对话中 → go_idle() |
| `DOUBAO_EVT_SESSION_CREATED` | 会话创建 | 释放缓冲音频 |
| `DOUBAO_EVT_TRANSCRIPT_DELTA` | 用户语音片段 | 更新用户气泡 |
| `DOUBAO_EVT_TRANSCRIPT_DONE` | 转录完成 | LISTENING → THINKING |
| `DOUBAO_EVT_OUTPUT_TEXT_DELTA` | AI 回复片段 | 流式显示 + 气泡追加 |
| `DOUBAO_EVT_OUTPUT_TEXT_DONE` | AI 回复完成 | 保存笔记 + 执行 [DEVICE:] 命令 |
| `DOUBAO_EVT_AUDIO_STARTED` | 音频开始 | 暂停采集 + 进入 SPEAKING |
| `DOUBAO_EVT_AUDIO_DELTA` | 音频片段 | 推入播放环形缓冲 |
| `DOUBAO_EVT_AUDIO_DONE` | 音频结束 | 状态码 20000002 → 退出；否则 drain |
| `DOUBAO_EVT_RESPONSE_DONE` | 响应完成 | 无音频时进入 AUTO_LISTEN |
| `DOUBAO_EVT_SESSION_CLOSED` | 会话关闭 | 非 IDLE → go_idle() |
| `DOUBAO_EVT_INTERRUPTED` | 打断确认 | SPEAKING/AUTO_LISTEN → LISTENING |
| `DOUBAO_EVT_ERROR` | 错误 | go_idle() + 错误日志 |

### 5.4 两层状态机的交互

```
Legacy 状态机 (UI 状态)          Doubao 对话状态机 (内部状态)
─────────────────────────       ─────────────────────────────
IDLE                         ←→ CHAT_IDLE
    │                              │
    │ 唤醒词/单击                   │ doubao_chat_start()
    ▼                              ▼
LISTENING                    ←→ CHAT_LISTENING
    │                              │ VAD commit
    ▼                              ▼
THINKING/STREAMING           ←→ CHAT_COMMITTING → CHAT_THINKING
    │                              │ audio.started
    ▼                              ▼
TTS_PLAYING                  ←→ CHAT_SPEAKING
    │                              │ audio.done + drain
    ▼                              ▼
IDLE                         ←→ CHAT_AUTO_LISTEN / CHAT_IDLE
```

**关键点**：
- Doubao 路径使用 `app_set_state()` 直接设置 UI 状态（绕过 Legacy 状态机的转移校验）
- Legacy 状态机的 `s_current_state` 通过 `app_state_machine_force_current()` 保持同步
- 资源仲裁仍由 Legacy 状态机管理

---

## 六、错误处理与恢复

### 6.1 错误场景处理

| 场景 | 处理策略 |
|------|----------|
| WSS 断线 | 指数退避 2→60s 重连；UI 显示"重连中"；带 session.id 续接；失败则新建会话 |
| API 错误事件 | error_log 记录 + UI 气泡提示；鉴权失败 → 提示检查 API Key |
| 限流/冷却 | 冷却 5s 自动恢复，UI 提示"稍后重试" |
| 音频设备异常 | codec 读写错误 → 重初始化音频子系统，不崩溃 |
| 内存保护 | 状态超时看门狗（思考 30s / 播报 60s 强制回收）+ 任务看门狗 + mem_monitor |
| API 超时 | commit 后 15s 无 output → 超时重连；播报中 5s 无 audio delta → 超时重连 |
| 服务端主动关闭会话 | 清除会话标志 + go_idle()；下次唤醒重建会话 |
| DMA 池耗尽 | I2S DMA 减少到 6 描述符；SPI max_transfer_sz 减少到 4KB |
| WebSocket 客户端崩溃 | 本地化补丁：destroy() 始终 join 任务；STOPBED_BIT 信号机制 |

### 6.2 连续错误恢复

```
连续 ERROR 状态计数（5 分钟窗口）
  │
  ├─ 计数 < 10 → 继续重试
  │
  ├─ 计数 ≥ 10 且 Web 服务器运行 → 重置计数，继续重试（允许用户修复配置）
  │
  └─ 计数 ≥ 10 且 Web 服务器未运行 → 进入深度睡眠
```

### 6.3 状态超时保护

| 状态 | 超时时间 | 处理 |
|------|----------|------|
| THINKING | 30s | 断线重连 + 回 IDLE |
| STREAMING | 30s | 回 IDLE |
| TTS_PLAYING | 60s | 停止 TTS |
| RESPONSE (UI) | 60s | 回 IDLE |
| commit 后 | 15s | 断线重连 + 回 IDLE |
| session.create | 5s | 回 IDLE |

---

## 七、配置管理

### 7.1 NVS 存储项

| 键 | 类型 | 默认值 | 说明 |
|----|------|--------|------|
| `wifi_ssid` | string | "" | WiFi SSID |
| `wifi_password` | string | "" | WiFi 密码 |
| `api_key` | string | "" | 豆包 API Key |
| `voice` | string | "zh_female_vv_jupiter_bigtts" | 音色 |
| `speed` | int8 | 0 | 语速 [-50,100] |
| `loudness` | int8 | 0 | 音量 [-50,100] |
| `system_prompt` | string | "你是一个桌面语音助手" | 系统提示词 |
| `auto_continue` | bool | true | 自动续听 |
| `idle_timeout_s` | uint8 | 8 | 自动续听空闲超时（秒） |
| `volume` | uint8 | 100 | 扬声器音量 0-100 |
| `brightness` | uint8 | 80 | 屏幕亮度 0-100 |
| `sleep_timeout_ms` | uint32 | 60000 | 休眠超时（ms），0=禁用 |
| `auto_read_response` | bool | true | 自动朗读回复 |
| `webserver_enabled` | bool | true | Web 服务器启用 |
| `silence_timeout_ms` | uint16 | 1500 | VAD 静音超时 |
| `max_record_seconds` | uint8 | 15 | 最长录音时间 |

### 7.2 Web 配置页面

- **地址**：`http://<device_ip>/`
- **功能**：WiFi SSID/密码、API Key、音色、语速、音量、系统提示词
- **安全**：API Key 打码显示 + 显示/隐藏切换；页面提示"仅可信局域网使用"
- **配网**：AP 模式 Captive Portal 自动重定向

---

## 八、数据持久化

### 8.1 聊天记录

- **存储位置**：`/sdcard/notes/chat_YYYY-MM-DD.json`
- **格式**：ISO 8601 时间戳 + user/assistant 角色 + 消息内容
- **限制**：每天最多 100 条，每条最多 4096 字符
- **用途**：重启后恢复最近一轮对话；AI 可查询历史记录

### 8.2 错误日志

- **存储方式**：RAM 中 32 条环形缓冲
- **严重级别**：INFO / WARNING / ERROR / CRITICAL
- **来源**：DEVICE / WIFI / STT / TTS
- **导出**：Web API `/api/errors` JSON 格式

---

## 九、用户交互

### 9.1 触摸手势

| 手势 | IDLE 态 | 聆听中 | 播报中 | 回复展示 |
|------|---------|--------|--------|----------|
| 单击 | 开始对话 | 取消 | 打断→聆听 | 关闭回复 |
| 双击 | — | — | 停止播报→IDLE | — |
| 长按 1s | 打开设置 | 打断→设置 | 打断→设置 | 打开设置 |

### 9.2 按键交互（BOOT 键）

| 状态 | 短按 | 长按 4s |
|------|------|---------|
| IDLE | 开始对话 | 深度睡眠 |
| LISTENING | 取消录制 | — |
| THINKING/STREAMING | 取消对话 | — |
| TTS_PLAYING | 打断→IDLE | — |
| RESPONSE | 打断→IDLE | — |

### 9.3 串口 CLI 命令

| 命令 | 说明 |
|------|------|
| `talk` / `t` | 触发对话 |
| `say <msg>` | 直接推送文本 |
| `abort` | 中断对话 |
| `status` / `s` | 显示设备状态 |
| `wifi <ssid> <pwd>` | 配置 WiFi |
| `web` | 切换 Web 服务器 |
| `doubao connect` | 连接豆包 |
| `doubao status` | 显示连接状态 |
| `mp3list` | 列出 MP3 文件 |
| `mp3play <file>` | 播放 MP3 |
| `mp3stop` | 停止播放 |
| `reboot` | 重启设备 |
| `tasks` | 显示任务列表 |
| `help` / `h` / `?` | 显示帮助 |

### 9.4 RGB LED 状态映射（Audio Board 变体）

| 状态 | LED 效果 | 颜色 |
|------|----------|------|
| BOOT/CONNECTING | Rainbow Spin | 彩虹 |
| IDLE | Solid | 绿色 (0,4,0) |
| LISTENING | Pulse Wave | 红色 (40,0,0) |
| SENDING | Chase | 蓝色 (0,16,40) |
| THINKING | Chase | 橙色 (40,24,0) |
| STREAMING | Sparkle | 紫色 (32,8,48) |
| RESPONSE | Solid | 绿色 (0,8,0) |
| TTS_LOADING | Chase | 青色 (0,24,24) |
| TTS_PLAYING | Breathe | 绿色 (0,32,16) |
| NOTIFYING | Breathe | 琥珀色 (40,32,0) |
| ERROR | Blink | 红色 (32,0,0) |

---

## 十、网络与安全

### 10.1 WiFi

- **STA 模式**：自动重连 + 指数退避
- **AP 模式**：Captive Portal 配网
- **PS 模式**：清醒时 MIN_MODEM，睡眠时 MAX_MODEM

### 10.2 WebSocket

- **端点**：`wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue`
- **鉴权**：`X-Api-Key` 请求头
- **Ping**：握手期 1s，连接后 30s
- **重连**：指数退避 2→60s；退避中用户唤醒 → 取消退避立即重连

### 10.3 TLS/证书

- 使用 `esp_crt_bundle_attach`（内置根证书库）
- SNTP 先校时（证书有效期校验）
- mbedTLS：开 `DYNAMIC_BUFFER`，关 `KEEP_PEER_CERTIFICATE`

### 10.4 API Key 安全

- **存储**：NVS（不硬编码在固件）
- **输入**：Web 界面（打码显示 + 显示/隐藏切换）
- **导出**：JSON API 打码处理
- **加密**：v2 路线图（NVS 加密 + Flash 加密）

---

## 十一、内存管理

### 11.1 关键规则

1. LVGL 绘制缓冲 → 内部 RAM（`MALLOC_CAP_INTERNAL`）
2. DMA 缓冲 → 内部 RAM（`MALLOC_CAP_DMA`）
3. 任务 TCB → 内部 RAM（栈可放 PSRAM）
4. 大缓冲 → 显式 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`
5. Flash 擦写期间 → PSRAM 不可访问

### 11.2 监控工具

- `mem_monitor` 任务：周期性堆快照
- 任务栈高水位监控
- 心跳日志（每 10s）
- `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` 内部堆监控

---

## 十二、测试策略

### 12.1 集成测试清单

1. 启动序列日志验收：Board → I2C → Codec → WiFi → SNTP → WSS → Ready
2. 单轮对话
3. 打断（barge-in）
4. 连续多轮
5. 清空对话
6. 断网重连
7. 唤醒词检测

### 12.2 全双工/回声专项测试

1. 播报中用户开口 → 本地快速停播（<100ms），服务端未被自身播报声误导
2. 高音量播报中无人说话 → VAD/能量检测不误触发打断
3. 长回复播报中连续多次打断 → 状态机稳定无卡死
4. 2 小时全双工运行 + 内存曲线 + 任务栈高水位监控

### 12.3 稳定性测试

- 2 小时连续运行
- 内存监控（无泄漏、无碎片失控）
- 任务栈高水位监控

---

## 十三、项目里程碑

| 阶段 | 内容 | 验收标准 |
|------|------|----------|
| M1 | 骨架移植：复制裁剪编译烧录 | 屏幕点亮、播放测试音、mic 采到音；全双工回路测试 |
| M2 | doubao_voice 协议链路 | WSS 连接、session 管理、文本流式收发 |
| M3 | 完整语音链路 | 上行 PCM+VAD、播报、打断、唤醒词集成 |
| M4 | UI 完善 | 气泡、设置页、屏保、中文字体、错误处理 |
| M5 | 测试打磨 | 集成/异常/稳定性测试、文档 |

---

## 十四、风险与后续优化

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 豆包错误码不完整 | 特定错误处理不精确 | v1 通用错误处理，遇到具体错误码再细化 |
| Opus 编解码缺失 | 带宽占用较高 | v2 优化项（speech_opus 上行 / ogg_opus 下行） |
| 回声风险 | 打断误触发 | 分层控制：播放感知门控 → 服务端鲁棒性 → esp-sr AEC |
| 重采样 CPU 开销 | 播报卡顿 | 3:2 多相重采样待实测；降级方案：带限线性插值 |
| API Key 安全 | 物理读取风险 | v1 = NVS + Web UI；v2 = NVS 加密 + Flash 加密 |
| HTTP 网页 | 局域网明文 | v1 仅提示可信网络；v2 = 自签名 HTTPS |

---

## 十五、依赖清单

| 组件 | 版本 | 用途 |
|------|------|------|
| ESP-IDF | v5.5.5 | RTOS & 外设框架 |
| LVGL | v9.5.0 | 嵌入式 GUI 库 |
| esp-sr | v1.9.5 | 语音识别（WakeNet） |
| esp_lvgl_port | v2.6.0 | LVGL-ESP 集成 |
| esp_codec_dev | v1.5.0 | 音频编解码驱动 |
| esp_websocket_client | 1.1.0 (patched) | WebSocket 客户端 |
| led_strip | v2.5.5 | WS2812 LED 控制 |
| cJSON | — | JSON 解析（PSRAM 钩子） |

---

## 十六、术语表

| 术语 | 说明 |
|------|------|
| WSS | WebSocket Secure，加密 WebSocket 连接 |
| VAD | Voice Activity Detection，语音活动检测 |
| ASR | Automatic Speech Recognition，自动语音识别 |
| TTS | Text-to-Speech，文本转语音 |
| AEC | Acoustic Echo Cancellation，声学回声消除 |
| PSRAM | Pseudo-Static RAM，伪静态随机存储器 |
| DMA | Direct Memory Access，直接内存访问 |
| NVS | Non-Volatile Storage，非易失性存储 |
| SNTP | Simple Network Time Protocol，简单网络时间协议 |
| Captive Portal | 强制门户，自动重定向到配网页面 |
| Barge-in | 打断，播报中用户说话触发中断 |

---

*文档版本: 1.0*
*最后更新: 2026-08-27*
