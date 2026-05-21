# AIWearable - AI 语音助手设计文档

> 后端基于 OpenClaw 云端服务，硬件基于微雪 ESP32-S3-Audio-Board + 1.47寸触摸屏

---

## 1. 产品概述

- **产品名称**: AIWearable
- **硬件平台**: 微雪 ESP32-S3-Audio-Board (16MB Flash, 8MB PSRAM) + 1.47寸触摸屏(172×320)
- **产品定位**: 语音优先的桌面 AI 智能终端，通过语音与云端 OpenClaw 服务端对话
- **后端**: OpenClaw Server (云服务器部署)
- **关键约束**: OpenClaw Gateway WebSocket 仅支持 JSON 文本帧，STT/TTS 在设备端通过 API 完成

---

## 2. 系统架构

### 2.1 架构总览 (方案A: ESP32 纯管道架构)

```
ESP32-S3                          Cloud
┌─────────────────────┐          ┌──────────────────────┐
│  [MIC] → [Fun-ASR]  │          │  OpenClaw Server     │
│           ↓          │   WSS   │  ┌────────────────┐  │
│       text ──────────┼─────────→│→│  AI Engine     │  │
│                      │←─────────│←│  Memory/Skills │  │
│       text ←─────────┼──────────│  └────────────────┘  │
│         ↓            │          │                      │
│    [MIMO TTS] → [SPK]│          │  LLM API             │
│                      │          └──────────────────────┘
│  [1.47" Display]     │
│  [Touch/Button/Wake] │
│  [7×RGB LEDs]        │
│  [SD Card]           │
│  [Camera (reserved)] │
└─────────────────────┘
```

### 2.2 数据流

1. 用户触发录音（唤醒词/按键/触摸）
2. ESP32 录音 → Fun-ASR API → 获得文本
3. 文本通过 WebSocket 发送到 OpenClaw
4. OpenClaw AI 处理 → 流式返回文本 (delta/final)
5. ESP32 接收文本 → MIMO TTS API → 播放语音
6. 对话文本追加写入 SD 卡聊天记录
7. AI 回复中的 `[DEVICE:...]` 标签解析执行（MP3控制/音量、屏幕亮度设置/聊天记录查询总结/其它引脚功能的调用）

### 2.3 OpenClaw 双向命令

```
ESP32 → OpenClaw:    用户语音文本
ESP32 → OpenClaw:    服务器设备状态 / 聊天记录响应
OpenClaw → ESP32:    AI 回复文本 (流式)
OpenClaw → ESP32:    [DEVICE:mp3=play:xxx]  播放音乐
OpenClaw → ESP32:    [DEVICE:mp3=pause|stop|next|prev]
OpenClaw → ESP32:    [DEVICE:volume=N]  设置音量
OpenClaw → ESP32:    [DEVICE:brightness=N]  设置亮度
OpenClaw → ESP32:    [DEVICE:chatlog=query:日期] 查询聊天记录
```

### 2.4 交互模式 (混合模式 D)

- **唤醒词触发**: "你好小智" 唤醒
- **物理按键触发**: BOOT 键 
- **触摸屏触发**: 点击屏幕按钮（可选）

---

## 3. 硬件规格

| 硬件 | 型号/规格 |
|------|----------|
| 主控 | ESP32-S3R8 (16MB Flash, 8MB PSRAM) |
| 屏幕 | 1.47" IPS LCD, 172×320, JD9853 驱动, SPI2 |
| 触摸 | AXS5106L 电容触摸, I2C (0x5C) |
| 音频输入 | ES7210 麦克风 ADC, I2S0, 16kHz/16bit |
| 音频输出 | ES8311 扬声器 DAC, I2S0, 16kHz/16bit |
| RGB LED | 7× WS2812 环形灯, GPIO38 |
| IO 扩展 | TCA9555, I2C (0x20) |
| SD 卡 | SDMMC 1-bit, FATFS |
| RTC | PCF85063, I2C (0x51) |
| Camera | 预留接口 (延迟初始化) |

### 3.1 引脚表 (已验证, 来自 AIClaw_Ver1.2)

```
I2C0:    SDA=11, SCL=10
SPI2:    CLK=4, MOSI=9, MISO=8, CS=3, DC=7, BL=5
I2S0:    MCLK=12, SCLK=13, LRCK=14, DSIN=15, DOUT=16
SDMMC:   CLK=40, CMD=42, D0=41
RGB LED: GPIO38
BOOT:    GPIO0
IO扩展(TCA9555):
  PA_EN=P1.0, BTN1=P1.1, BTN2=P1.2, BTN3=P1.3
  LCD_RST=P0.0, TOUCH_RST=P0.1, TOUCH_INT=P0.2, SD_CS=P0.3
  CAM_EN=P0.5, CAM_MUX=P0.6 (预留)
```

---

## 4. ESP32 固件模块设计

### 4.1 项目结构 (基于 AIClaw_Ver1.2 移植)

```
main/                          # 主程序
  app_main.cpp                 # 入口 + 主事件循环
  app_state.cpp                # 共享状态 + 设备指令解析
  voice_chat.cpp               # 录音→STT→OpenClaw 对话流程
  app_tasks.cpp                # 后台任务管理
  serial_cmd.cpp               # 串口调试命令
components/
  board/                       # 板级抽象 (引脚/外设驱动)
  openclaw/                    # OpenClaw WebSocket + ED25519 认证
  stt/                         # 百炼 Fun-ASR 客户端
  tts/                         # 小米 MIMO TTS 客户端
  ui/                          # LVGL UI (15状态 + GIF + RGB LED)
  wifi_manager/                # WiFi STA/AP + Captive Portal 配网
  webserver/                   # HTTP API + Web 管理界面
  settings/                    # NVS 持久化配置
  error_log/                   # 环形缓冲错误日志
  mp3_player/                  # SD 卡 MP3 解码播放
  wake_word/                   # WakeNet 唤醒词检测
  notes_manager/               # SD 卡聊天记录管理
  camera/                      # 摄像头 (预留)
```

### 4.2 模块职责

| 模块 | 职责 |
|------|------|
| board/ | GPIO/外设抽象，板级初始化，所有驱动已验证 |
| openclaw/ | WebSocket 客户端、ED25519 设备认证、JSON-RPC 协议 |
| stt/ | 百炼 Fun-ASR WebSocket 语音识别客户端 |
| tts/ | 小米 MIMO TTS HTTP 语音合成客户端 |
| ui/ | LVGL 界面、15状态状态机、GIF动画、RGB LED 控制 |
| wifi_manager/ | WiFi STA 连接/AP 配网模式管理 |
| webserver/ | HTTP API + 配网页 + 文件管理界面 |
| mp3_player/ | SD 卡 MP3 解码播放，与 TTS 互斥 |
| wake_word/ | WakeNet 唤醒词检测，与录音互斥 |
| notes_manager/ | 聊天记录追加写入/读取 SD 卡 |

### 4.3 驱动原则

- 所有外设驱动从 AIClaw_Ver1.2 移植，已验证，不自写驱动协议层
- 屏幕: JD9853 (SPI2)，非 ST7789
- 触摸: AXS5106L (I2C 0x5C)
- 音频: ES8311 DAC + ES7210 ADC 共享 I2S0

---

## 5. 音频管道设计

### 5.1 STT: 百炼 Fun-ASR

- 协议: WebSocket (wss://dashscope.aliyuncs.com)
- 模型: fun-asr-realtime
- 采样率: 16kHz, 16-bit mono
- 客户端: 移植自 `components/stt/`

### 5.2 TTS: 小米 MIMO

- 协议: HTTP POST
- 端点: https://api.xiaomimimo.com/v1/chat/completions
- 客户端: 移植自 `components/tts/`

### 5.3 音频互斥规则

- TTS 播放与 MP3 播放互斥 (共享 I2S + ES8311)
- 录音前停止 TTS (释放内部 RAM)
- 录音时暂停 LVGL 刷新 (释放内部 RAM 给 STT)
- 唤醒词检测与录音互斥 (共享 I2S ADC)

---

## 6. 显示与 UI

### 6.1 屏幕布局 (172×320)

```
┌─────────────────────┐
│ WiFi 时间 OpenClaw  │  ← 状态栏 (20px)
├─────────────────────┤
│                     │
│  [GIF 角色状态]      │  ← 中央区 (245px)
│  gif人物状态切换     │    boot/idle/listening/thinking/
│                     │     speaking/playing/error
│                     │  
│                     │
├─────────────────────┤
│  [IDLE] Standby     │  ← 状态英文 (25px)
├─────────────────────┤
│       空闲          │  ← 状态中文 (30px)
└─────────────────────┘
```

### 6.2 15状态状态机

```
SLEEP → ARMED → BOOT → CONNECTING → IDLE → LISTENING → SENDING
  ↑                                               ↓
  │                                          THINKING
  │                                              ↓
  │                                          STREAMING
  │                                              ↓
  └──(60s无交互)──────────────── RESPONSE ←──────┘
                            ↓
              TTS_LOADING → SPEAKING
                            ↓
              PLAYING_MP3 (音乐播放)
              ERROR (故障)
```

### 6.3 GIF 状态映射

| 状态 | GIF 文件 | RGB LED |
|------|---------|---------|
| BOOT | boot.gif | 橙色渐变 |
| IDLE | idle.gif | 暗绿色 |
| LISTENING | listening.gif | 红色脉冲 |
| THINKING | thinking.gif | 蓝色呼吸 |
| STREAMING | thinking.gif | 紫色 |
| SPEAKING | speaking.gif | 绿色呼吸 |
| PLAYING_MP3 | playing.gif | 彩虹律动 |
| ERROR | error.gif | 红色闪烁 |

### 6.4 7×RGB LED 灯效

- 使用已验证的 WS2812 驱动 (GPIO38, RMT)
- 每种状态独立配色+动画模式
- 支持: SOLID / BREATHE / BLINK / RAINBOW / CHASE

---

## 7. SD 卡数据布局

```
SD卡根目录/
├── chat/                         # 聊天记录 (每天一个文件)
│   ├── 2026-05-19.txt
│   └── 2026-05-20.txt
├── music/                        # MP3 音乐
├── gifs/                         # GIF 表情动画
│   ├── idle.gif
│   ├── listening.gif
│   ├── thinking.gif
│   ├── speaking.gif
│   ├── playing.gif
│   ├── boot.gif
│   └── error.gif
└── system/                       # 系统配置备份
```

### 7.1 聊天记录格式

```
[14:30:05] USER: 今天天气怎么样
[14:30:08] ASSISTANT: 今天天气晴朗，最高温度28°C
```

### 7.2 聊天记录查询流程

1. 用户对 OpenClaw 说"总结昨天的对话"
2. OpenClaw 回复中包含 `[DEVICE:chatlog=query:2026-05-19]`
3. ESP32 读取 `/chat/2026-05-19.txt`
4. ESP32 通过 `chat.send` 将内容发回 OpenClaw
5. OpenClaw AI 总结后返回

---

## 8. Web 管理功能

### 8.1 基础功能 (移植自 AIClaw_Ver1.2)

| 端点 | 说明 |
|------|------|
| `GET /` | Web 管理界面 |
| `GET /api/status` | 设备状态快照 (WiFi/OC/内存/任务) |
| `GET /api/errors` | 错误环形缓冲 |
| `GET /api/settings` | 读取设置 |
| `POST /api/settings` | 修改设置 |
| `POST /api/reboot` | 重启设备 |
| `POST /api/provision` | 配网信息保存 |

### 8.2 新增功能

| 端点 | 说明 |
|------|------|
| `GET /api/chat/history?date=YYYY-MM-DD` | 聊天记录预览，支持分页 |
| `POST /api/mp3/upload` | MP3 文件上传 (multipart) |
| `POST /api/mp3/delete` | MP3 文件删除 |
| `POST /api/gif/upload` | GIF 文件上传，校验分辨率 ≤172×172 |
| `POST /api/gif/delete` | GIF 文件删除 |

### 8.3 配网流程

1. WiFi 连接失败 10 次 → 自动进入 AP 模式 (SSID: `AIWearable_Config`)
2. 手机连接热点 → Captive Portal 自动弹出
3. 手动访问 `http://192.168.4.1`
4. 填写 WiFi + OpenClaw + API Key → 保存 → 设备重启

---

## 9. 约束与规范

### 9.1 内存约束

- 音频缓冲 >4KB → PSRAM (MALLOC_CAP_SPIRAM)
- LVGL 显示缓冲 → 内部 SRAM (DMA 兼容)
- 网络任务栈 ≥8192 字节
- 音频/LVGL 任务栈 ≥4096 字节
- TTS 与 MP3 播放互斥
- 录音时暂停 LVGL 刷新释放内部 RAM

### 9.2 编码规范 (继承 AIClaw CODING_STANDARD)

- 全局变量 `g_` 前缀，静态变量 `s_` 前缀
- 公共函数 `模块名_动词_名词` 格式
- 宏/枚举 全大写 + 模块前缀
- LVGL 非渲染线程操作必须 `lvgl_port_lock()/unlock()`
- `components/` 不依赖 `main/`
- UI ↔ Main 使用事件组 + 队列通信

### 9.3 网络约束

- WebSocket JSON 文本帧，`id` 必须字符串
- 握手阶段 ping_interval=1s，正常 30s
- `final` 覆盖 `delta` 缓冲 (防重复)
- 所有网络发送字符串 UTF-8 验证
- ED25519 设备身份认证，密钥存储 NVS 不输出日志

### 9.4 日志规范

- 每个模块固定 TAG
- 状态转移日志: `from → to` + 触发原因
- OpenClaw RPC: id/method/耗时
- 内存监控: 关键操作前后打印堆使用
- 严禁输出密钥/Token/密码

---

## 10. 开发任务清单

### 阶段 1: 项目初始化与基础驱动移植

- [ ] 搭建 ESP-IDF v5.5 开发环境，验证编译脚本
- [ ] 创建 AIWearable 项目骨架，移植 AIClaw_Ver1.2 components/
- [ ] 验证板级抽象: I2C/I2S/SPI/SDMMC 初始化
- [ ] 验证 TCA9555 IO 扩展器，PA_EN/SD_CS/复位引脚
- [ ] 验证 JD9853 屏幕点亮 (SPI2)
- [ ] 验证 AXS5106L 触摸响应 (I2C)
- [ ] 验证 WS2812 RGB LED (GPIO38, 7灯)
- [ ] 验证 SD 卡 SDMMC 1-bit 挂载 (FATFS)
- [ ] 验证 LVGL 8.x 显示缓冲 + 触摸输入

### 阶段 2: 音频子系统移植

- [ ] 验证音频编解码器: ES8311 播放 + ES7210 录音 (I2S0)
- [ ] 移植验证 STT 客户端: 百炼 Fun-ASR WebSocket
- [ ] 移植验证 TTS 客户端: 小米 MIMO TTS HTTP
- [ ] 移植验证 MP3 播放器: SD 卡解码 + TTS/MP3 互斥
- [ ] 移植 WakeNet 唤醒词 + 录音互斥

### 阶段 3: 网络与 OpenClaw 集成

- [ ] 移植验证 WiFi 管理: STA/AP 模式 + Captive Portal 配网
- [ ] 移植验证 OpenClaw 客户端: ED25519 认证 + WebSocket
- [ ] 移植验证设备控制标签 `[DEVICE:...]` 解析
- [ ] 验证 SNTP 时间同步

### 阶段 4: Web 管理增强

- [ ] 移植验证基础 HTTP API (status/errors/settings)
- [ ] 实现聊天记录预览 API: `/api/chat/history?date=&page=`
- [ ] 实现 MP3 文件管理: 上传/删除 API + 前端 UI
- [ ] 实现 GIF 文件管理: 上传(校验分辨率)/删除 API + 前端 UI

### 阶段 5: GIF 表情 + 聊天记录

- [ ] 启用 LVGL GIF 支持 (LV_USE_GIF=y)
- [ ] 实现各状态 GIF 加载/切换
- [ ] 移植验证聊天记录追加写入
- [ ] 实现 OpenClaw 查询聊天记录 → 总结流程

### 阶段 6: 系统管理 + 摄像头预留

- [ ] 验证睡眠/唤醒: 20s退出激活 + 60s休眠 + BOOT键唤醒
- [ ] 验证 RGB 灯效: 各状态独立配色/动画
- [ ] 预留摄像头接口 (延迟初始化)

### 阶段 7: 联调测试

- [ ] 端到端语音对话测试
- [ ] 音乐播放控制测试
- [ ] 聊天记录写入+查询+AI总结测试
- [ ] Web 管理功能测试
- [ ] 30分钟稳定性测试 + 内存监控
- [ ] 网络中断/SD插拔异常测试

---

## 11. 参考资料

- AIClaw_Ver1.2 参考工程: `/home/ubuntu/esp32/AIClaw_Ver1.2/`
- OpenClaw 协议文档: https://docs.openclaw.ai/
- 微雪 ESP32-S3-Audio-Board: https://www.waveshare.com/wiki/ESP32-S3-AUDIO-Board
