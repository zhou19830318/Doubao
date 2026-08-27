# 豆包语音机器人

> **语言**: [English](README.md) | 简体中文

基于 ESP32-S3 的开源 AI 语音对话机器人，通过单条 WebSocket 全双工连接与豆包（字节跳动）实时语音 API 通信，实现"开口即说、随时打断"的自然对话体验。

[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5%2B-blue)](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/index.html)
[![Platform](https://img.shields.io/badge/Platform-ESP32--S3-red)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Stage](https://img.shields.io/badge/Stage-DVT-orange)]()

---

## 项目概述

豆包语音机器人将 ESP32-S3 开发板变成一款语音优先的 AI 伴侣。通过单条 WebSocket 连接与[火山引擎豆包](https://www.volcengine.com/product/doubao)实时语音 API 通信，在一条连接内完成 ASR + LLM + TTS 全链路处理。

```mermaid
flowchart TD
    subgraph Robot["豆包语音机器人"]
        direction TB
        
        subgraph Logic["上层逻辑"]
            A["唤醒词<br>(ESP-SR)"] --> B["豆包对话<br>引擎"]
            B <--> C["豆包语音 API<br>(WSS 全双工)"]
        end
        
        subgraph AudioIn["音频输入"]
            D["I2S 麦克风<br>(ES7210)"] --> A
            E["VAD<br>(16kHz)"] --> B
        end
        
        subgraph AudioOut["音频输出"]
            C --> F["I2S 扬声器<br>(ES8311)"]
        end
        
        subgraph Other["其他外设与功能"]
            G["LVGL UI<br>(AMOLED)"]
            H["MP3<br>播放器"]
            I["Web<br>配置页"]
            J["唤醒词<br>(\"你好小智\")"]
        end
    end

    C --> K["云端"]
    
    %% 样式调整
    classDef default fill:#fff,stroke:#333,stroke-width:1px;
    classDef cloud fill:#f9f9f9,stroke:#666,stroke-dasharray: 5 5;
    class K cloud;
```
---

## 核心特性

### 全双工语音对话
- **单条 WSS 连接**：ASR + LLM + TTS 全在一条 WebSocket 连接内完成
- **实时打断**：播报中可随时开口打断 — 本地能量检测，6dB 余量
- **连续对话**：响应完成后自动续听，可配置空闲超时（默认 8 秒）
- **多轮上下文**：服务端托管 20 轮上下文

### 唤醒词与 VAD
- **离线唤醒词**：ESP-SR WakeNet 引擎 — "你好小智"（可配置）
- **智能 VAD**：自适应噪声底校准，静音检测（默认 1.5 秒，最长 15 秒）
- **反馈音效**：唤醒时"叮咚"，取消时"咚叮"

### 触摸与按键交互
- **单击**：开始对话 / 打断播报
- **双击**：停止播报
- **长按 1 秒**：打开设置页
- **BOOT 键**：对话 / 取消 / 深度睡眠

### 音频
- **全双工 I2S**：ES7210 采集 + ES8311 播放同时工作，16kHz
- **MP3 播放器**：SD 卡 MP3 播放，支持播放列表
- **3:2 重采样**：服务端输出 24kHz，硬件播放 16kHz — 多相重采样

### 设备控制
- **AI 控制设备**：AI 可通过 `[DEVICE:]` 指令调节音量、亮度、播放音乐、控制 RGB 灯效
- **语音指令**："音量调到 50" → `[DEVICE:volume=50]`

### 用户界面
- **AMOLED 显示屏**：410×502 AMOLED，LVGL 9.x 深色主题
- **对话气泡**：用户（右侧）和 AI（左侧）气泡，流式文本显示
- **状态指示器**：颜色编码状态（绿色=待机，红色=聆听，橙色=思考等）

---

## 硬件规格

### 主控板：Waveshare ESP32-S3-Touch-AMOLED-2.06

| 组件 | 规格 |
|------|------|
| MCU | ESP32-S3R8（双核 Xtensa LX7 @ 240MHz） |
| Flash | 32MB (QSPI) |
| PSRAM | 8MB (Octal SPI) |
| 显示屏 | 410×502 QSPI AMOLED (CO5300/SH8601) |
| 触摸 | FT3168 电容式 (I2C) |
| 音频 DAC | ES8311（扬声器） |
| 音频 ADC | ES7210（四通道麦克风） |
| IMU | QMI8658 六轴 |
| PMU | AXP2101 |
| SD 卡 | microSD (SPI) |
| 按键 | BOOT (GPIO0) |

### 也支持
- Waveshare ESP32-S3-AUDIO-Board（1.47" IPS LCD，7× WS2812 RGB 灯环）
- M5StickC Plus2（旧版支持）
- SenseCAP Watcher（旧版支持）

---

## 快速开始

### 前置条件

- **ESP-IDF v5.5+** 及 ESP32-S3 工具链
- **豆包 API Key**（从[火山引擎控制台](https://console.volcengine.com/)获取）
- Waveshare ESP32-S3-Touch-AMOLED-2.06（或支持的开发板）
- USB-C 数据线、microSD 卡（FAT32 格式）

### 编译与烧录

```bash
# 克隆仓库
git clone <repo-url> doubao-voice-robot
cd doubao-voice-robot

# 设置目标芯片
idf.py set-target esp32s3

# 配置（选择开发板、唤醒词等）
idf.py menuconfig

# 编译
idf.py build

# 烧录并监控
idf.py -p /dev/ttyACM0 -b 921600 flash monitor
```

### 首次配置

1. **WiFi**：设备未配置 WiFi 时以 AP 模式启动
   - 连接 `Doubao_Config` 热点
   - 浏览器自动弹出配置页面 → 输入 WiFi 凭据

2. **API Key**：通过网页界面或串口 CLI
   ```bash
   # 串口 CLI
   Doubao> web    # 启动 Web 服务器
   # 然后在浏览器打开 http://<设备IP>
   ```

3. **唤醒词**：说"你好小智"开始对话

---

## 状态机机制

应用采用 **15 态有限状态机**，分为两层：

### UI 状态（Legacy 状态机）

| 状态 | 颜色 | 说明 |
|------|------|------|
| `SLEEP` | 暗灰 | 深度睡眠 |
| `ARMED` | 靛蓝 | 唤醒词待命 |
| `BOOT` | 橙色 | 系统启动中 |
| `CONNECTING` | 蓝色 | WiFi/WSS 连接中 |
| `IDLE` | 绿色 | 待机，等待唤醒词 |
| `LISTENING` | 红色 | 录制用户语音 |
| `SENDING` | 蓝色 | 上传音频 |
| `THINKING` | 橙色 | 等待 AI 回复 |
| `STREAMING` | 紫色 | 接收流式文本 |
| `RESPONSE` | 绿色 | 回复已展示 |
| `TTS_LOADING` | 青色 | 加载音频 |
| `TTS_PLAYING` | 绿色 | 播放 AI 回复 |
| `PLAYING_MP3` | 青色 | SD 卡 MP3 播放 |
| `NOTIFYING` | 橙色 | 通知 |
| `ERROR` | 红色 | 错误 |

### 内部对话状态（豆包对话引擎）

| 状态 | 说明 |
|------|------|
| `CHAT_IDLE` | 等待启动 |
| `CHAT_LISTENING` | 麦克风采集，VAD 活跃 |
| `CHAT_COMMITTING` | commit 已发送，等待响应 |
| `CHAT_THINKING` | 服务端处理中（已收到转录） |
| `CHAT_SPEAKING` | 音频 delta 播放中 |
| `CHAT_AUTO_LISTEN` | 响应完成，自动续听 |

### 对话流程

```
IDLE ──(唤醒词/单击)──▶ LISTENING ──(VAD 静音)──▶ COMMITTING
                                                         │
                          AUTO_LISTEN ◀──(音频完成)◀── SPEAKING
                               │                           ▲
                               │ (用户说话)                │
                               └───────────────────────────┘
                               
IDLE ◀──(超时 8s)── AUTO_LISTEN
IDLE ◀──(退出意图/20000002)── SPEAKING
IDLE ◀──(双击/打断)── 任意状态
```

### 资源仲裁

| 资源 | 占用状态 | 优先级 |
|------|----------|--------|
| `RES_AUDIO_IN` (I2S RX) | LISTENING | 最高 |
| `RES_AUDIO_OUT` (I2S TX) | LISTENING, TTS_PLAYING, PLAYING_MP3, NOTIFYING | 可抢占 |

高优先级状态可抢占低优先级状态的资源。

---

## 配置管理

### 通过 Web 界面（推荐）

1. 启动 Web 服务器：`Doubao> web` 或 `[DEVICE:webserver=on]`
2. 在浏览器打开 `http://<设备IP>`
3. 配置：WiFi、API Key、音色、语速、音量、系统提示词

### 通过串口 CLI

```bash
Doubao> help              # 显示所有命令
Doubao> status            # 设备状态
Doubao> talk              # 开始语音对话
Doubao> say 你好 AI       # 发送文本给 AI
Doubao> doubao status     # 连接状态
Doubao> reboot            # 重启设备
```

### 通过语音指令

| 指令 | 说明 |
|------|------|
| "音量调到50" | 设置音量为 50% |
| "调亮一点" | 增加亮度 |
| "播放音乐" | 显示播放列表 / 播放音乐 |
| "停止音乐" | 停止 MP3 播放 |

---

## AI 设备控制指令

AI 可通过 `[DEVICE:command]` 标签控制设备：

| 指令 | 说明 |
|------|------|
| `[DEVICE:volume=0-100]` | 设置音量 |
| `[DEVICE:brightness=0-100]` | 设置亮度 |
| `[DEVICE:mp3=play:<file>]` | 播放 MP3 文件 |
| `[DEVICE:mp3=stop]` | 停止播放 |
| `[DEVICE:mp3=show]` | 显示播放列表 |
| `[DEVICE:mp3=index:N]` | 按序号播放 |
| `[DEVICE:rgb=rainbow/aurora/fire/ocean]` | RGB 灯效控制 |
| `[DEVICE:sleep=N]` | 设置休眠超时（分钟） |
| `[DEVICE:reboot]` | 重启设备 |
| `[DEVICE:webserver=on/off]` | 网页服务器开关 |
| `[DEVICE:auto_read=on/off]` | 自动朗读开关 |

---

## 项目结构

```
doubao-voice-robot/
├── main/                          # 应用核心
│   ├── app_main.c                 # 系统初始化 & 事件循环
│   ├── app_state.c                # 全局状态、设备指令
│   ├── app_state_machine.c        # 状态机 & 资源仲裁
│   ├── app_tasks.c                # 后台任务
│   ├── doubao_chat.c              # 对话引擎（VAD、打断）
│   ├── serial_cmd.c               # 串口 CLI
│   ├── mem_monitor.c              # 内存监控
│   └── include/                   # 头文件
│
├── components/                    # 可复用组件
│   ├── board/                     # 硬件抽象（多板卡支持）
│   ├── doubao_voice/              # 豆包 WSS 全双工客户端
│   ├── ui/                        # LVGL 用户界面
│   ├── wifi_manager/              # WiFi + Captive Portal
│   ├── settings/                  # NVS 持久化配置
│   ├── webserver/                 # HTTP 配置服务器
│   ├── wake_word/                 # ESP-SR 唤醒词检测
│   ├── mp3_player/                # SD 卡 MP3 播放器
│   ├── notes_manager/             # 聊天记录 SD 卡存储
│   ├── error_log/                 # 错误日志
│   └── esp_websocket_client/      # 补丁版 WebSocket 客户端
│
├── docs/                          # 文档
├── PRD/                           # 产品需求
├── partitions.csv                 # Flash 分区表
├── sdkconfig.defaults             # SDK 配置默认值
└── CMakeLists.txt                 # 构建系统
```

---

## Flash 分区布局（32MB）

| 分区 | 大小 | 用途 |
|------|------|------|
| nvs | 24KB | 配置存储 |
| phy_init | 4KB | PHY 校准 |
| factory | 8MB | 应用固件 |
| model | 960KB | 唤醒词模型（WakeNet） |
| storage | 2MB | 应用数据与日志 |

---

## 依赖项

| 组件 | 版本 | 用途 |
|------|------|------|
| ESP-IDF | v5.5.5 | RTOS 及外设框架 |
| LVGL | v9.5.0 | GUI 库 |
| esp-sr | v1.9.5 | 唤醒词（WakeNet） |
| esp_lvgl_port | v2.6.0 | LVGL-ESP 集成 |
| esp_codec_dev | v1.5.0 | 音频编解码驱动 |
| doubao_voice | 自定义 | 豆包 WSS 客户端 |

---

## 许可证

MIT 许可证 - 详见 [LICENSE](LICENSE)

```
SPDX-FileCopyrightText: 2024-2026 Doubao Contributors
SPDX-License-Identifier: MIT
```

---

## 致谢

- **[火山引擎豆包](https://www.volcengine.com/product/doubao)** — 实时语音对话 API
- **[乐鑫科技](https://www.espressif.com/)** — ESP-IDF 框架及 ESP-SR
- **[微雪电子](https://www.waveshare.com/)** — ESP32-S3 开发板
- **[LVGL](https://lvgl.io/)** — 嵌入式图形库

---

*由 Doubao Contributors 用 ❤️ 构建*
