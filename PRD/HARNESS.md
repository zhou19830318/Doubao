# HeyClawy 开发约束文件 (HARNESS) v2.0

本文件定义了针对“微雪 ESP32-S3-Audio-Board + 1.47寸触摸屏”硬件组合的开发环境、硬件引脚约束、依赖关系以及资源限制。所有开发工作必须严格遵守此约束。

## 1. 开发环境约束
- **框架版本**：必须使用 **ESP-IDF v5.5** 或更高版本。
- **操作系统**：支持 Linux, Windows 或 macOS。推荐使用 VS Code + ESP-IDF 插件。
- **编译脚本**：
  - 严禁直接在根目录运行 `idf.py build`。
  - **必须**使用工程提供的构建脚本 `./build_audio_board.sh` (Linux/macOS) 或 `build_audio_board.bat` (Windows)。该脚本会自动处理 `sdkconfig` 替换和板级宏 `CONFIG_HEYCLAWY_BOARD_WAVESHARE_AUDIO` 的配置。

## 2. 硬件资源与引脚分配约束 (Pinmap)

### 2.1 总线复用约束
- **I2C0 (频率默认 400kHz，可降到 100kHz)**
  - SDA: `GPIO_NUM_11`
  - SCL: `GPIO_NUM_10`
  - **挂载设备**：TCA9555 IO扩展 (0x20)、ES8311 DAC (0x18)、ES7210 ADC (0x40)、AXS5106L 触摸屏 (0x5C)、PCF85063 RTC (0x51)。
  - **约束**：在多任务操作 I2C 设备（如触摸轮询与读取 IO 扩展状态）时，底层驱动必须使用互斥锁确保 I2C 总线线程安全。

- **SPI2 (主机模式)**
  - CLK: `GPIO_NUM_4`, MOSI: `GPIO_NUM_9`, MISO: `GPIO_NUM_8`
  - **挂载设备**：JD9853 LCD 屏幕。
  - **约束**：LVGL 刷新任务拥有较高优先级，SPI 传输采用 DMA 模式，需注意单次传输大小不能超过 DMA 限制。

- **I2S0 (音频总线)**
  - MCLK: `GPIO_NUM_12`, SCLK: `GPIO_NUM_13`, LRCK: `GPIO_NUM_14`
  - DOUT: `GPIO_NUM_16` (发送至 ES8311 扬声器)
  - DSIN: `GPIO_NUM_15` (接收自 ES7210 麦克风)
  - **约束**：由于麦克风与扬声器共用同一 I2S 控制器，采样率和位宽必须保持一致 (默认 16kHz, 16-bit)。

### 2.2 离散控制引脚约束
- **LCD 控制**：CS (`GPIO3`), DC (`GPIO7`), BL (`GPIO5`)。
- **RGB 灯带**：`GPIO38` (单线 RMT 或 SPI 驱动 WS2812)。
- **SD 卡**：SDMMC 1-bit 模式 (CLK=`GPIO40`, CMD=`GPIO42`, D0=`GPIO41`)。
- **BOOT 按键**：`GPIO0`（用于从睡眠进入 ARMED；也可作为紧急打断）。

### 2.3 IO 扩展芯片 (TCA9555) 约束
底板引脚不足，部分信号通过 TCA9555 控制：
- **Port 0 (P0.0 - P0.7)**
  - LCD_RST (P0.0)
  - TOUCH_RST (P0.1)
  - TOUCH_INT (P0.2)：触摸中断（低有效输入）
  - SD_CS (P0.3)：SD 卡 CS（在 SDMMC 1-bit 模式下必须保持高电平，避免误入 SPI 模式）
  - 其余位：按参考工程可能存在预留/可选功能（例如 camera enable/mux）
- **Port 1 (P1.0 - P1.7)**
  - PA_EN (P1.0)：扬声器功放使能。**音频播放前必须拉高，播放结束后拉低以消除底噪。**
  - USER_BUTTONS (P1.1 - P1.3)：配置为输入，低有效
  - 其余位：预留

## 3. 内存与性能约束

### 3.1 内存分配 (PSRAM)
- 硬件拥有 8MB PSRAM。
- **音频缓存**：所有 TTS/STT 录音和播放的大型 Buffer (大于 4KB) **必须**使用 `MALLOC_CAP_SPIRAM` 分配在 PSRAM 中。
- **LVGL 缓存**：显示 Buffer 推荐分配在内部 SRAM 配合 DMA，以提高刷新帧率。但若内部内存不足，可部分放置于 PSRAM 中。

### 3.2 任务栈大小
- 涉及网络请求 (如 WebSocket, HTTP Server, DashScope STT) 的任务，栈大小不得低于 `8192` 字节。
- 音频解码和 LVGL 刷新任务，栈大小不得低于 `4096` 字节。

### 3.3 业务互斥约束 (Critical)
- **音频互斥**：MP3 音乐播放与 TTS 语音播报绝对不能同时进行。它们共享相同的 ES8311 解码器、I2S 通道及堆内存。
- 接收到 MP3 播放指令或操作 MP3 UI 时，必须中止当前 TTS 播放。

### 3.4 唤醒与录音互斥约束 (Critical)
- **I2S 争用禁止**：唤醒检测、录音、TTS/MP3 播放在任何时刻只能由一个“音频主流程”占用 I2S。
- **唤醒暂停**：进入录音前必须暂停 WakeNet；录音结束后再恢复（避免误触发与音频读写冲突）。

## 4. 网络与 OpenClaw 约束 (Critical)

### 4.1 连接与协议
- **连接方式**：设备通过 WebSocket 连接云服务器上的 OpenClaw Gateway（优先使用 `wss://`）。
- **帧格式**：仅允许 JSON 文本帧，禁止发送二进制音频。
- **请求 ID**：所有 RPC 的 `id` 必须是字符串；不得使用数字。
- **缓冲与栈**：WebSocket 接收缓冲不低于 8192 字节；认证阶段 ED25519 签名需要较大的任务栈（网络任务栈不低于 8192 字节）。

### 4.2 连接存活与超时
- **Ping/Keepalive**：认证握手阶段需要更频繁的 ping，避免使用默认 10 秒超时导致认证失败。
- **重连策略**：断线必须自动重连，重连需指数退避并限制最大频率，避免云端限流。

### 4.3 流式响应去重
- OpenClaw `chat` 事件的 `delta` 为增量片段，`final` 为完整消息。
- **约束**：收到 `final` 必须覆盖（replace）已累积的 `delta` 缓冲，禁止追加以避免“重复文本”。

## 5. SD 卡与文件系统约束

### 5.1 目录与文件命名
- 聊天日志目录：`/chat/`，每日一个文件：`YYYY-MM-DD.txt`。
- 音乐目录：`/music/`，存放 `*.mp3`。

### 5.2 写入与磨损控制
- **追加写**：聊天日志必须以追加方式写入，避免频繁重写整文件。
- **刷盘策略**：在每条消息写入后允许短周期 flush，但必须限制频率（例如合并写入或按时间窗口刷盘）以降低磨损。
- **掉电安全**：写入失败时不得阻塞主流程；需要可观测的错误状态，并允许继续对话（日志可降级为内存缓存）。

## 6. 密钥与安全约束
- 编译前**必须**手动从 `secrets.h.example` 复制并创建 `main/include/secrets.h` 文件。
- `secrets.h` 和 `secrets.txt` 严禁提交至版本控制系统。
- 设备身份私钥 (`SECRETS_DEVICE_KEY_HEX`) 需妥善保管，若未配置，系统首次启动将自动生成随机密钥并存入 NVS。

## 7. 依赖组件
- **UI 库**：`lvgl` (建议版本 8.3.x)
- **语音唤醒**：`esp-sr` (唤醒词网络)
- **屏幕驱动**：`esp_lcd` 及配套供应商驱动 (`esp_lcd_panel_jd9853`)
- **网络栈**：ESP-IDF 默认 LwIP, `esp_websocket_client`, `esp_http_server`

## 8. 状态机与交互超时约束
- 设备必须实现统一状态机，并在屏幕底栏可视化：`SLEEP / ARMED / IDLE / LISTENING / THINKING / SPEAKING / PLAYING_MP3`。
- **退出激活**：进入激活后，20 秒无有效对话必须退出到 `ARMED/IDLE`。
- **进入睡眠**：超过 60 秒无有效交互进入 `SLEEP`（息屏 + RGB 降亮）。
- **睡眠唤醒**：BOOT 键按下只能进入 `ARMED`（待激活），不得直接开始录音。

## 9. 触控交互约束
- 触摸事件必须通过 UI→Main 事件解耦机制上报（组件不得直接依赖 `main/`）。
- 触摸取消行为必须具备幂等性：重复取消不会导致状态机紊乱或资源重复释放。

## 9.1 Step 8 - OpenClaw WebSocket断开(code=1007)和内存分配失败修复 (2026-05-12)

### 现象
1. ESP32与OpenClaw服务器通信时频繁断开，日志显示 `WS CLOSE frame: code=1007`
2. TTS播放过程中发生内存分配失败导致系统重启：`Mem alloc fail. size 0x0000065e caps 0x0000080c`
3. 从飞书发送消息到HeyClawy未收到响应
4. SD卡MP3文件名显示为乱码（如 `▒▒ȻHR~1.MP3`）

### 根本原因分析
1. **WebSocket code=1007**：表示"无效帧数据"，通常是因为发送了包含null字节或其他无效UTF-8字符的JSON数据
2. **内存分配失败**：TTS播放时需要大量内部RAM用于SSE流解析和PCM缓冲，但内部RAM被其他模块占用导致耗尽
3. **SD卡文件名乱码**：FAT文件系统的8.3短文件名使用OEM代码页（codepage 437），不是有效的UTF-8编码，直接发送到OpenClaw会导致JSON解析失败

### 解决方案

#### 9.1.1 OpenClaw客户端UTF-8验证增强
**文件**: `components/openclaw/openclaw_client.c`

**修改点1**: 在发送connect请求前验证JSON字符串
```c
char *json_str = cJSON_PrintUnformatted(root);
if (json_str) {
    int jlen = strlen(json_str);
    /* Validate JSON string is valid UTF-8 before sending */
    bool valid_utf8 = true;
    for (int i = 0; i < jlen; i++) {
        unsigned char c = (unsigned char)json_str[i];
        if (c == 0) {
            ESP_LOGE(TAG, "JSON contains null byte at position %d - aborting send", i);
            valid_utf8 = false;
            break;
        }
    }
    
    if (valid_utf8) {
        esp_websocket_client_send_text(s_oc.ws, json_str, jlen, pdMS_TO_TICKS(5000));
    }
    free(json_str);
}
```

**修改点2**: 在接收消息时验证UTF-8
```c
static void handle_message(const char *data, int len)
{
    /* Validate UTF-8 before parsing to prevent code=1007 disconnects */
    if (len > 0 && data) {
        for (int i = 0; i < len; i++) {
            unsigned char c = (unsigned char)data[i];
            if (c == 0 && i < len - 1) {
                ESP_LOGW(TAG, "Invalid message: null byte at position %d", i);
                return;
            }
        }
    }
    // ... rest of function
}
```

#### 9.1.2 MP3列表UTF-8清理和PSRAM分配
**文件**: `main/app_state.c`

**问题**: SD卡MP3文件名是OEM编码，直接用于JSON会导致WebSocket断开

**解决方案**:
1. 使用已有的 `utf8_sanitize()` 函数清理文件名
2. 将大JSON缓冲区分配到PSRAM而非内部RAM

```c
} else if (strcmp(val, "scan") == 0 || strcmp(val, "list") == 0) {
    sd_mp3_cache_refresh();
    if (s_sd_mp3_count > 0) {
        /* Use PSRAM for large JSON buffer to avoid internal RAM exhaustion */
        char *list_json = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (list_json) {
            int off = snprintf(list_json, 2048,
                "{\"title\":\"SD卡MP3 (%d首)\",\"type\":\"list\",\"data\":[", s_sd_mp3_count);
            for (int i = 0; i < s_sd_mp3_count && off < 1950; i++) {
                /* Sanitize filename for JSON - ensure valid UTF-8 */
                char safe_name[MP3_FILE_NAME_MAX];
                strncpy(safe_name, s_sd_mp3_names[i], sizeof(safe_name) - 1);
                safe_name[sizeof(safe_name) - 1] = '\0';
                utf8_sanitize(safe_name);
                
                off += snprintf(list_json + off, 2048 - off,
                    "%s\"%d. %s\"", i > 0 ? "," : "", i + 1, safe_name);
            }
            // ... rest
        }
    }
}
```

#### 9.1.3 音频缓冲区早期释放
**文件**: `main/voice_chat.c`

**问题**: 录音缓冲区占用PSRAM直到STT完成，导致TTS播放时内存不足

**解决方案**: 在STT转录前尽早释放音频缓冲区

```c
/* Free audio buffer early to release PSRAM before STT/OpenClaw operations */
free(audio_buf);
audio_buf = NULL;

ESP_LOGI(TAG, "Heap after freeing audio: internal=%d SPIRAM=%d",
         heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

app_set_state(UI_STATE_SENDING);
ui_set_status_message("Transcribing...");
```

#### 9.1.4 增强的重连机制
**文件**: `main/app_tasks.c`

**改进**:
1. 重连前先强制断开以清理状态
2. 添加错误检查和详细日志
3. 记录堆内存使用情况用于调试

```c
if (reconnect_ticks * 500 >= backoff_ms) {
    reconnect_ticks = 0;
    ESP_LOGW(TAG, "OpenClaw reconnect (backoff=%dms, consecutive_errors=%d)", 
             backoff_ms, openclaw_get_info()->ok ? 0 : 1);
    
    /* Force disconnect before reconnect to clean up state */
    openclaw_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    esp_err_t ret = openclaw_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OpenClaw connect failed: %s", esp_err_to_name(ret));
    }
    // ... exponential backoff
}
```

**文件**: `components/openclaw/openclaw_client.c`

在断开连接时记录堆内存信息：
```c
case WEBSOCKET_EVENT_DISCONNECTED:
    s_oc.consecutive_errors++;
    ESP_LOGW(TAG, "WebSocket disconnected (state=%d, consecutive_errors=%d)",
             s_oc.state, s_oc.consecutive_errors);
    
    if (s_oc.consecutive_errors > 3) {
        ESP_LOGW(TAG, "Multiple disconnects detected - checking connection stability");
        ESP_LOGW(TAG, "Heap: internal=%d SPIRAM=%d",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
    // ...
```

### 教训与最佳实践

1. **UTF-8验证是关键**：所有通过网络发送的字符串必须验证UTF-8有效性，特别是来自文件系统的内容
2. **PSRAM vs 内部RAM**：大缓冲区（>4KB）应优先使用PSRAM，保留内部RAM给关键任务（TTS、网络栈）
3. **早期释放资源**：不再需要的缓冲区应立即释放，特别是在内存紧张的操作序列中
4. **防御性编程**：在发送数据前验证，在接收数据后验证，避免脏数据传播
5. **详细的错误日志**：记录堆内存、连续错误次数等上下文信息，便于远程诊断

### 验证步骤

1. 编译并烧录固件：`./build_audio_board.sh flash monitor`
2. 观察启动日志，确认无内存分配失败
3. 触发语音对话，检查OpenClaw连接稳定性
4. 执行 `[DEVICE:mp3=scan]` 命令，验证MP3列表正常显示无乱码
5. 长时间运行测试（>30分钟），监控内存使用和重连频率

### 相关文件
- `components/openclaw/openclaw_client.c`: WebSocket客户端，UTF-8验证
- `main/app_state.c`: MP3列表处理，UTF-8清理
- `main/voice_chat.c`: 音频录制和STT，内存管理
- `main/app_tasks.c`: 重连看门狗，错误处理

## 10. 高内聚低耦合架构约束 (Critical)

### 10.1 模块边界
- **单一职责**：每个模块必须能用一句话描述其职责，并只对该职责负责。
- **禁止跨层调用**：底层驱动不得直接操作 UI；业务逻辑不得直接写硬件寄存器。
- **板级抽象唯一入口**：所有 GPIO/外设差异必须封装在 `board_*` 抽象中，禁止在业务模块硬编码引脚号。

### 10.2 依赖方向（防循环依赖）
- `components/` 不得依赖 `main/`。
- `main/` 可以依赖 `components/`。
- UI 与 Main 交互必须使用事件组/队列等“消息机制”，或使用 setter 注入句柄（避免 include 形成环）。

### 10.3 共享状态与事件
- 共享状态必须集中定义在单一头文件（例如 `app_state.h`）中，避免多处重复定义。
- Event bits 必须有“唯一权威定义源”，UI 侧如需复制常量，必须保证数值一致并在变更时同步。

## 11. 可观测性与快速定位调试约束 (Critical)

### 11.1 日志必须包含的定位信息
- **模块 TAG**：每个源文件必须有固定 TAG。
- **状态转移日志**：每次状态机切换必须打印 `from -> to` 以及触发原因（例如 `wakeword/touch_cancel/ws_chat_final/mp3_play`）。
- **OpenClaw 请求链路**：每个 RPC 必须打印 `id(字符串)`、`method`、发送时间；收到 `res`/`event` 必须打印 `id/event/state` 以及耗时（毫秒）。
- **音频互斥链路**：音频资源抢占/释放必须打印 `owner`、采样率/通道、失败原因（例如 `busy/preempted`）。
- **存储链路**：SD 挂载/写入失败必须打印 `path`、`esp_err_to_name()`、重试/降级策略是否触发。

### 11.2 错误日志（强制）
- 关键模块错误必须写入统一 error log（环形缓冲），并可通过 Web API 导出查看（避免“只在串口一闪而过”）。
- 错误条目必须包含：时间戳、来源（WIFI/OPENCLAW/STT/TTS/SD/UI/DEVICE）、严重级别、128 字符内摘要。
- 严禁在日志中输出密钥/Token/密码；需要掩码显示。

### 11.3 现场自检能力（快速回归）
- 串口必须提供最小自检命令：`status`（WiFi/OpenClaw/heap/task 状态）、`web`（WebServer 开关）、`reboot`、`abort`（终止当前对话）。
- WebServer `/api/status` 必须能输出设备与 OpenClaw 状态快照；`/api/errors` 必须输出错误环形缓冲。

### 11.4 关键已知问题的防回归要求
- **认证握手超时**：WebSocket 在握手/重连的 `CONNECTED` 阶段必须将 `ping_interval_sec` 设为 `1`，避免 10 秒挑战消息被缓存导致握手超时。
- **Chat 文本重复**：收到 `final` 必须覆盖已累积的 `delta` 缓冲，禁止追加。

## 12. 参考资料索引（PRD/Project_Resources 强制对齐）

### 12.1 架构与解耦
- `PRD/Project_Resources/modular-architecture.md`：模块职责拆分、共享状态与事件模式
- `PRD/Project_Resources/ui-event-decoupling.md`：UI→Main 事件解耦（setter pattern）
- `PRD/Project_Resources/webserver-settings.md`：Settings/WebServer/ErrorLog 的解耦与接口设计

### 12.2 OpenClaw 协议与稳定性
- `PRD/Project_Resources/openclaw-api.md`：Gateway WebSocket API、事件类型、重要约束（JSON-only）
- `PRD/Project_Resources/openclaw-chat-response-handling.md`：delta/final 去重与状态回写保护
- `PRD/Project_Resources/ws-auth-ping-fix.md`：握手阶段 ping 问题与重连修复
- `PRD/Project_Resources/openclaw-device-auth.md`：ED25519 设备身份鉴权细节
- `PRD/Project_Resources/openclaw-security-patch.md`：ws:// 私网限制补丁（云端/局域网部署时参考）

### 12.3 音频、唤醒与功耗
- `PRD/Project_Resources/wake-word-detection.md`：WakeNet 选型与唤醒/录音互斥
- `PRD/Project_Resources/tts-playback.md`：TTS 播放链路注意事项
- `PRD/Project_Resources/power-consumption.md`：功耗与节能建议
- `PRD/Project_Resources/sleep-management.md`：睡眠/唤醒状态管理参考

### 12.4 调试与测试
- `PRD/Project_Resources/serial-monitoring.md`：串口监控与常见问题
- `PRD/Project_Resources/integration-test.md`：端到端回归测试清单

### 12.5 参考工程（引脚/驱动对照）
- `/home/ubuntu/esp32/VoiceClaw_Ver0.2/`：Waveshare 1.47" 触摸屏 + 音频板驱动参考
- `/home/ubuntu/esp32/HeyClawy/`：板级抽象、OpenClaw 接入、WebServer/Settings/ErrorLog 参考

## 13. 调试记录与优化经验

### 13.1 Step 9 - 唤醒词检测后内存分配失败导致重启修复 (2026-05-12)

**现象描述：**
1. 唤醒词检测成功：“你好小智”
2. TTS播放完成后，内部RAM仅剩 16443 字节
3. STT连接时发生内存分配失败：`Mem alloc fail. size 0x00000642 caps 0x0000080c`
4. 设备反复重启，无法进行语音对话

**根本原因分析：**

1. **TTS和STT同时占用大量内部RAM**
   - TTS播放使用PSRAM存储PCM数据，但播放任务本身需要内部RAM
   - STT的WebSocket客户端task_stack=6144字节占用内部RAM
   - 两者同时运行时内部RAM耗尽

2. **唤醒词触发时未停止TTS**
   - 用户说话时TTS可能还在播放之前的响应
   - 直接启动STT导致两个大内存模块并发运行

3. **缺少内存状态监控**
   - 关键操作前没有检查可用内存
   - 无法及时发现内存紧张状态

**详细解决方案：**

#### 方案1：在唤醒词回调中强制停止TTS

修改 `main/app_main.c` 第600行附近：
```c
if (ev & WAKE_WORD_BIT) {
    app_reset_activity_timer();
    ESP_LOGI(TAG, "Wake word detected!");
    ui_state_t cur = ui_get_state();
    if (cur == UI_STATE_IDLE || cur == UI_STATE_RESPONSE) {
        /* CRITICAL: Stop TTS immediately to free internal RAM for STT */
        if (tts_is_playing()) {
            ESP_LOGI(TAG, "Stopping TTS due to wake word");
            tts_stop();
            vTaskDelay(pdMS_TO_TICKKS(50));
        }
        
        wake_word_pause();
        voice_chat_start();
        wake_word_resume();
    }
}
```

#### 方案2：在voice_chat_start开始时再次检查并停止TTS

修改 `main/voice_chat.c` 第73行附近：
```c
void voice_chat_start(void)
{
    if (openclaw_get_state() != OPENCLAW_STATE_CONNECTED) {
        // ... existing code ...
    }

    /* CRITICAL: Stop any ongoing TTS to free internal RAM before recording.
     * TTS and STT both need large internal RAM buffers - running them
     * simultaneously causes heap exhaustion and crashes. */
    if (tts_is_playing()) {
        ESP_LOGI(TAG, "Stopping TTS before voice chat to free memory");
        tts_stop();
        g_tts_pending = false;
        /* Give TTS task time to clean up */
        vTaskDelay(pdMS_TO_TCKS(100));
    }

    const settings_t *cfg = settings_get();

    /* Log memory state before starting */
    ESP_LOGI(TAG, "Memory before voice_chat: internal=%d SPIRAM=%d",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // ... rest of function ...
}
```

#### 方案3：增强tts_stop()确保状态重置

修改 `components/tts/tts_client.c` 第594行：
```c
void tts_stop(void)
{
    s_tts.stop_requested = true;
    int timeout = 50;
    while (s_tts.playing && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TCKS(50));
    }
    
    /* Reset state for next TTS session */
    if (!s_tts.playing) {
        s_tts.stop_requested = false;
    }
}
```

#### 方案4：优化STT WebSocket配置减少内部RAM占用

修改 `components/stt/stt_client.c` 第199行：
```c
esp_websocket_client_config_t ws_cfg = {
    .uri = s_stt.cfg.endpoint[0] ? s_stt.cfg.endpoint : "wss://dashscope.aliyuncs.com/api-ws/v1/inference/",
    .headers = auth_header,
    .crt_bundle_attach = esp_crt_bundle_attach,
    .task_stack = 6144,
    .buffer_size = 2048,
    .user_agent = "HeyClawy/1.0",
    .network_timeout_ms = 20000,
    .disable_auto_reconnect = true,
    /* Use global CA store to reduce internal RAM usage */
    .use_global_ca_store = true,
};
```

**教训与最佳实践：**

1. **TTS和STT互斥原则**
   - 永远不要让TTS和STT同时运行
   - 在启动录音前必须确认TTS已完全停止
   - 使用双重检查机制（唤醒词回调 + voice_chat_start）

2. **内存监控策略**
   - 在关键操作前后记录内部RAM和PSRAM使用情况
   - 设置阈值告警：内部RAM < 20KB时警告
   - 日志格式：`ESP_LOGI(TAG, "Memory: internal=%d SPIRAM=%d", ...)`

3. **任务栈内存管理**
   - WebSocket、HTTP等大内存任务尽量使用PSRAM
   - 通过配置选项控制任务栈分配位置
   - 定期检查任务实际栈使用量（uxTaskGetStackHighWaterMark）

4. **状态清理的重要性**
   - tts_stop()不仅要停止播放，还要重置状态标志
   - 避免下次调用时因状态混乱导致问题
   - 所有stop函数都应包含完整的状态清理逻辑

5. **防御性编程**
   - 在资源紧张场景下，宁可多检查一次
   - 添加vTaskDelay给任务清理留出时间
   - 关键路径上添加详细的诊断日志

**验证步骤：**
1. 编译并烧录固件
2. 等待TTS播放完成
3. 说出唤醒词“你好小智”
4. 观察日志确认：
   - TTS被正确停止
   - 内部RAM充足（> 20KB）
   - STT连接成功
   - 无内存分配失败
5. 连续测试5次以上，确保稳定性

**相关文件列表：**
- `main/app_main.c`：唤醒词回调处理
- `main/voice_chat.c`：语音聊天流程控制
- `components/tts/tts_client.c`：TTS播放实现
- `components/stt/stt_client.c`：STT客户端实现

### 13.2 Step 10 - LVGL暂停优化和SD卡文件名乱码修复 (2026-05-12)

**现象描述：**
1. 录音时仍然出现内存分配失败（内部RAM仅剩18KB）
2. SD卡MP3文件名显示为乱码：`▒▒ȻHR~1.MP3`
3. OpenClaw通信正常，但语音对话无法进行

**根本原因分析：**

1. **LVGL刷新任务占用内部RAM**
   - LVGL在录音期间持续刷新屏幕，占用约10-15KB内部RAM
   - 与STT WebSocket任务（6KB栈）叠加后导致内存不足
   - 之前的TTS停止优化还不够，需要进一步优化UI任务

2. **FAT文件系统编码问题**
   - SD卡使用FAT文件系统，文件名使用OEM代码页（GBK/CP437）
   - ESP32默认使用UTF-8编码
   - 直接读取的文件名包含非UTF-8字节，导致JSON传输时出现乱码
   - 乱码可能导致OpenClaw服务器拒绝接收（code=1007）

**详细解决方案：**

#### 方案1：录音前暂停LVGL任务

修改 `main/voice_chat.c` 第98行附近：
```c
/* CRITICAL: Pause LVGL task to free internal RAM for STT WebSocket.
 * LVGL refresh uses ~10-15KB internal RAM. Pausing it during recording
 * prevents heap exhaustion when STT WebSocket task starts. */
extern esp_err_t lvgl_port_stop(void);
extern esp_err_t lvgl_port_start(void);
lvgl_port_stop();
ESP_LOGI(TAG, "LVGL paused to save internal RAM");
```

#### 方案2：录音结束后恢复LVGL任务

在三个位置添加`lvgl_port_start()`调用：
1. 录音正常结束后（第347行）
2. 用户取消录音时（第173行）
3. 录音太短被丢弃时（第337行）

```c
/* Resume LVGL task now that recording is done and memory is freed */
lvgl_port_start();
ESP_LOGI(TAG, "LVGL resumed");
```

#### 方案3：SD卡文件名UTF-8清理

在 `components/mp3_player/mp3_player.c` 中添加utf8_sanitize函数：
```c
static char *utf8_sanitize(char *s)
{
    for (char *p = s; *p; ) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x80) {
            p++;  /* ASCII — always valid */
        } else if ((c & 0xE0) == 0xC0) {
            /* 2-byte sequence */
            if (((unsigned char)p[1] & 0xC0) != 0x80) { *p = '?'; p++; }
            else p += 2;
        } else if ((c & 0xF0) == 0xE0) {
            /* 3-byte sequence */
            if (((unsigned char)p[1] & 0xC0) != 0x80 ||
                ((unsigned char)p[2] & 0xC0) != 0x80) { *p = '?'; p++; }
            else p += 3;
        } else if ((c & 0xF8) == 0xF0) {
            /* 4-byte sequence */
            if (((unsigned char)p[1] & 0xC0) != 0x80 ||
                ((unsigned char)p[2] & 0xC0) != 0x80 ||
                ((unsigned char)p[3] & 0xC0) != 0x80) { *p = '?'; p++; }
            else p += 4;
        } else {
            *p = '?'; p++;  /* Invalid start byte */
        }
    }
    return s;
}
```

在扫描函数中使用：
```c
/* Sanitize filename to valid UTF-8 (FAT uses OEM codepage) */
strncpy(file_names[count], entry->d_name, MP3_FILE_NAME_MAX - 1);
file_names[count][MP3_FILE_NAME_MAX - 1] = '\0';
utf8_sanitize(file_names[count]);
```

**教训与最佳实践：**

1. **多模块并发时的内存管理**
   - TTS、STT、LVGL同时运行时内部RAM极易耗尽
   - 采用“暂停-恢复”策略：录音时暂停LVGL，释放内部RAM给STT
   - 关键路径上添加详细的内存监控日志

2. **文件系统编码兼容性**
   - FAT/exFAT文件系统使用OEM代码页，不是UTF-8
   - 所有从SD卡读取的字符串都需要UTF-8验证和清理
   - 使用`utf8_sanitize()`函数将无效字节替换为'?'

3. **防御性编程**
   - 在所有退出路径上都恢复LVGL（正常结束、取消、错误）
   - 避免资源泄漏和状态不一致
   - 使用extern声明访问其他模块的函数

4. **OpenClaw协议稳定性**
   - JSON中不能包含无效UTF-8字符
   - 文件名乱码会导致WebSocket断开（code=1007）
   - 发送前必须验证所有字符串的UTF-8有效性

**验证步骤：**
1. 编译并烧录固件
2. 观察启动日志，确认MP3文件名不再乱码
3. 说出唤醒词开始录音
4. 观察日志确认：
   - “LVGL paused to save internal RAM”
   - 录音时内部RAM > 25KB
   - “LVGL resumed”
   - 无内存分配失败
5. 测试MP3扫描功能，确认文件名正确显示
6. 连续测试10次以上，确保稳定性

**相关文件列表：**
- `main/voice_chat.c`：LVGL暂停/恢复逻辑
- `components/mp3_player/mp3_player.c`：UTF-8清理函数

## 14. 文档管理规范 (Critical)

### 14.1 禁止随意创建新的 .md 文档
- **原则**：严禁在项目中随意创建新的 .md 文档文件
- **原因**：
  - 避免文档碎片化，保持项目结构清晰
  - 减少维护成本，防止文档过时或重复
  - 确保重要信息集中在核心文档中
  
### 14.2 文档更新策略
- **优先更新现有文档**：当需要添加新内容时，首先考虑更新现有的相关文档
- **HARNESS.md**：用于记录开发约束、技术规范、调试经验等重要信息
- **README.md**：项目概述和快速入门指南
- **PRD.md**：产品需求文档
- **组件文档**：各组件的详细说明应放在对应组件目录下

### 14.3 特殊情况处理
- **确需新建文档时**：必须经过团队讨论并获得批准
- **临时文档**：如需创建临时分析文档，应在任务完成后及时清理或整合到正式文档中
- **文档归档**：过时的文档应移动到归档目录或删除，而不是保留在项目根目录

### 14.4 当前允许的文档位置
- `/PRD/` - 产品需求和项目资源文档
- `/docs/` - 用户文档和使用指南
- 各组件目录下的 README 或说明文件
- 根目录仅保留：README.md, HARNESS.md, PRD.md 等核心文档

### 14.5 违规示例
❌ 错误做法：
```bash
# 不要这样做
create_file("NEW_FEATURE.md")
create_file("DEBUG_NOTES.md") 
create_file("TEST_RESULTS.md")
```

✅ 正确做法：
```bash
# 应该这样做
# 1. 更新 HARNESS.md 中的相关章节
# 2. 在 PRD.md 中添加新功能描述
# 3. 在对应组件目录更新 README
# 4. 如确需新文档，先团队讨论确认
```

## 15. MP3文件扫描动态适配规范 (2026-05-14)

### 15.1 问题背景
原代码中存在硬编码的MP3文件数量限制（最多20个文件），导致当SD卡中有超过20个MP3文件时，只能扫描到前20个。

### 15.2 解决方案
实现了动态内存分配的MP3文件扫描功能，支持无限数量的MP3文件（仅受限于可用内存）。

### 15.3 核心修改

#### 15.3.1 mp3_player组件增强
**文件**: `components/mp3_player/include/mp3_player.h` 和 `components/mp3_player/mp3_player.c`

**新增函数**:
```c
esp_err_t mp3_player_scan_sd_dynamic(const char *directory, 
                                     char ***out_files, 
                                     uint16_t *out_count);
```

**特性**:
- 使用两次遍历方法：第一次统计文件数量，第二次分配精确大小的内存
- 返回动态分配的字符串数组，调用者负责释放内存
- 完整的错误处理和内存清理机制

**保留原有函数**: `mp3_player_scan_sd()` 保持向后兼容性

#### 15.3.2 app_state.c 修改
**数据结构变更**:
```c
// 原来: static char s_sd_mp3_names[20][MP3_FILE_NAME_MAX];
// 现在: static char **s_sd_mp3_names = NULL;
```

**扫描逻辑更新**:
- 使用动态扫描函数替代固定大小扫描
- 增加内存清理逻辑，避免内存泄漏
- 扩大列表缓冲区从1024到2048字节以支持更多文件

**新增清理函数**: `app_sd_mp3_cleanup()`
- 在应用退出时释放所有动态分配的内存

#### 15.3.3 ui_mp3.c 修改
**数据结构变更**:
```c
// 原来: static char s_file_names[MAX_FILE_LIST][FILE_NAME_LEN];
// 现在: static char **s_file_names = NULL;
```

**UI刷新逻辑**:
- `ui_mp3_show()`: 使用动态扫描获取文件列表
- `ui_mp3_refresh_file_list()`: 支持动态数组的复制和管理
- 完善的内存分配和释放机制

#### 15.3.4 serial_cmd.c 修改
**串口命令优化**:
- `mp3list` 命令改用动态扫描
- 正确的内存释放流程

### 15.4 使用示例

#### 基本用法
```c
#include "mp3_player.h"

// 动态扫描SD卡中的MP3文件
char **files = NULL;
uint16_t count = 0;

esp_err_t ret = mp3_player_scan_sd_dynamic("/sdcard", &files, &count);

if (ret == ESP_OK && files) {
    printf("Found %d MP3 files:\n", count);
    
    // 遍历所有文件
    for (int i = 0; i < count; i++) {
        printf("  [%d] %s\n", i, files[i]);
    }
    
    // 重要：释放内存
    for (int i = 0; i < count; i++) {
        free(files[i]);  // 释放每个文件名
    }
    free(files);  // 释放指针数组
}
```

#### 应用退出时清理
```c
// 在应用关闭前调用
app_sd_mp3_cleanup();
```

### 15.5 技术优势

1. **无数量限制**: 支持任意数量的MP3文件（受限于内存）
2. **内存效率**: 只分配实际需要的内存空间
3. **向后兼容**: 保留原有API，不影响其他模块
4. **健壮性**: 完善的错误处理和内存管理
5. **可扩展性**: 易于添加更多功能（如子目录递归扫描）

### 15.6 注意事项

1. **内存管理**: 
   - 必须释放内存：每次调用 `mp3_player_scan_sd_dynamic()` 后，必须释放返回的内存
   - 释放顺序：先释放每个字符串，再释放指针数组
   - 避免重复释放：确保只释放一次

2. **性能考虑**: 
   - 两次遍历目录可能略慢于单次遍历
   - 建议缓存：对于频繁访问的场景，建议使用 `app_state.c` 中的缓存机制

3. **线程安全**: 
   - 当前实现不是线程安全的，建议在单个任务中使用

### 15.7 相关文件
- `components/mp3_player/include/mp3_player.h`: 新增动态扫描函数声明
- `components/mp3_player/mp3_player.c`: 动态扫描函数实现
- `main/app_state.c`: 使用动态数组存储文件列表，增加清理函数
- `main/include/app_state.h`: 新增清理函数声明
- `components/ui/ui_mp3.c`: UI界面支持动态文件列表
- `main/serial_cmd.c`: 串口命令支持动态扫描

## 16. GIF动图播放功能评估 (2026-05-14)

### 16.1 评估目标
评估 https://github.com/derdacavga/Gif-Player 仓库是否适合在AIClaw项目的各个UI状态下播放GIF动图。

### 16.2 技术方案对比

#### 方案A: derdacavga/Gif-Player (Arduino + TFT_eSPI)
**技术栈**:
- Arduino IDE框架
- TFT_eSPI显示库
- AnimatedGIF解码库
- ILI9341显示屏驱动

**优点**:
1. ✅ 高性能：可达30 FPS流畅播放
2. ✅ PSRAM优化：利用8MB PSRAM做帧缓冲
3. ✅ SD卡直接读取：无需预加载到内存
4. ✅ 触摸控制：完整的UI交互

**缺点**:
1. ❌ **不兼容ESP-IDF框架**：项目使用Arduino，而AIClaw使用ESP-IDF v5.5
2. ❌ **不使用LVGL**：AIClaw已深度集成LVGL 8.x，切换成本高
3. ❌ **硬件不匹配**：针对ILI9341，而AIClaw使用JD9853 (1.47寸触摸屏)
4. ❌ **SPI引脚冲突**：与现有硬件引脚分配不一致
5. ❌ **需要重写驱动层**：TFT_eSPI → LVGL移植工作量大

#### 方案B: LVGL内置GIF支持 (推荐)
**技术栈**:
- ESP-IDF v5.5框架
- LVGL 8.x图形库（已集成）
- lv_gif模块（已包含在managed_components中）
- gifdec解码器

**优点**:
1. ✅ **完全兼容**：与现有技术栈无缝集成
2. ✅ **零额外依赖**：LVGL已包含gifdec和lv_gif模块
3. ✅ **统一渲染**：与现有UI组件一致
4. ✅ **易于维护**：官方支持，文档完善
5. ✅ **文件系统支持**：可读取SD卡或PSRAM中的GIF

**缺点**:
1. ⚠️ 性能略低于专用方案（约15-20 FPS，取决于GIF复杂度）
2. ⚠️ 需要启用LV_USE_GIF配置

### 16.3 当前项目状态分析

#### 硬件兼容性
- **显示屏**: JD9853 (1.47寸, 172x320分辨率) ✅
- **PSRAM**: 8MB可用 ✅
- **SD卡**: FAT32文件系统 ✅
- **SPI总线**: SPI2用于LCD，已有DMA支持 ✅

#### 软件状态
```json
// build/config/sdkconfig.json
"LV_USE_GIF": false  // ❌ 当前未启用
```

**LVGL版本**: 8.x (managed_components/lvgl__lvgl)
**GIF模块位置**: `managed_components/lvgl__lvgl/src/extra/libs/gif/`
- `lv_gif.h/c`: LVGL GIF控件封装
- `gifdec.h/c`: GIF解码器核心

### 16.4 推荐实施方案

#### 步骤1: 启用LVGL GIF支持
修改 `sdkconfig.defaults`:
```
CONFIG_LV_USE_GIF=y
CONFIG_LV_MEM_CUSTOM=y
```

重新编译:
```bash
./build_audio_board.sh clean
./build_audio_board.sh build
```

#### 步骤2: 创建GIF状态管理器
新增文件: `components/ui/ui_state_gif.h` 和 `ui_state_gif.c`

```c
/* ui_state_gif.h */
#pragma once
#include "lvgl.h"
#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/** GIF状态类型 */
typedef enum {
    GIF_STATE_BOOT,        /* 启动动画 */
    GIF_STATE_IDLE,        /* 待机动画 */
    GIF_STATE_LISTENING,   /* 监听动画 */
    GIF_STATE_THINKING,    /* 思考动画 */
    GIF_STATE_SPEAKING,    /* 说话动画 */
    GIF_STATE_ERROR,       /* 错误动画 */
    GIF_STATE_COUNT
} gif_state_type_t;

/** 初始化GIF状态管理器 */
esp_err_t ui_state_gif_init(void);

/** 切换到指定状态的GIF */
void ui_state_gif_show(gif_state_type_t state);

/** 隐藏当前GIF */
void ui_state_gif_hide(void);

/** 从SD卡加载GIF文件 */
esp_err_t ui_state_gif_load_from_sd(const char *filename, gif_state_type_t state);

#ifdef __cplusplus
}
#endif
```

#### 步骤3: GIF文件组织
SD卡目录结构:
```
/sdcard/
└── gifs/
    ├── boot.gif         /* 启动动画 */
    ├── idle.gif         /* 待机动画 */
    ├── listening.gif    /* 监听动画 */
    ├── thinking.gif     /* 思考动画 */
    ├── speaking.gif     /* 说话动画 */
    └── error.gif        /* 错误动画 */
```

**GIF规格建议**:
- 分辨率: 最大 172x172 (适应屏幕宽度)
- 帧率: 15-20 FPS (平衡性能和流畅度)
- 颜色: 256色或更少
- 文件大小: 单个GIF < 500KB
- 总大小: 所有GIF < 3MB

#### 步骤4: 集成到状态机
修改 `main/app_state.c`:

```c
void app_set_state(ui_state_t st)
{
    ui_set_state(st);
    app_led_for_state(st);
    
    /* 根据状态切换GIF动画 */
#if CONFIG_LV_USE_GIF
    switch (st) {
    case UI_STATE_BOOT:
        ui_state_gif_show(GIF_STATE_BOOT);
        break;
    case UI_STATE_IDLE:
        ui_state_gif_show(GIF_STATE_IDLE);
        break;
    case UI_STATE_LISTENING:
        ui_state_gif_show(GIF_STATE_LISTENING);
        break;
    case UI_STATE_THINKING:
        ui_state_gif_show(GIF_STATE_THINKING);
        break;
    case UI_STATE_TTS_PLAYING:
        ui_state_gif_show(GIF_STATE_SPEAKING);
        break;
    case UI_STATE_ERROR:
        ui_state_gif_show(GIF_STATE_ERROR);
        break;
    default:
        ui_state_gif_hide();
        break;
    }
#endif
}
```

#### 步骤5: 性能优化

**内存管理**:
```c
/* lv_conf.h 或通过menuconfig设置 */
#define LV_IMG_CACHE_DEF_SIZE 4  /* 缓存4个GIF帧 */
#define LV_MEM_SIZE (64 * 1024U) /* 增加LVGL内存池 */
```

**降低刷新频率**:
```c
/* 在GIF定时器中控制帧率 */
lv_timer_set_period(gif_timer, 50); /* 20 FPS */
```

**暂停非活动GIF**:
```c
/* 当GIF不可见时暂停播放 */
void ui_state_gif_hide(void)
{
    if (current_gif_obj) {
        lv_obj_add_flag(current_gif_obj, LV_OBJ_FLAG_HIDDEN);
        /* 可选：停止定时器以节省CPU */
    }
}
```

### 16.5 实施风险评估

#### 高风险项
1. **内存不足**: GIF解码需要额外内存
   - 缓解：限制GIF尺寸和颜色数，使用PSRAM
   
2. **性能下降**: GIF播放可能影响其他任务
   - 缓解：降低帧率，使用定时器优先级控制

3. **闪烁问题**: GIF与LVGL刷新冲突
   - 缓解：使用双缓冲，同步刷新时机

#### 中风险项
1. **SD卡读取延迟**: 大GIF文件加载慢
   - 缓解：预加载到PSRAM，或使用小文件

2. **状态切换卡顿**: GIF加载需要时间
   - 缓解：异步加载，显示过渡动画

#### 低风险项
1. **兼容性问题**: LVGL GIF模块成熟稳定
2. **维护成本**: 官方支持，社区活跃

### 16.6 替代方案考虑

如果LVGL GIF性能不满足需求，可以考虑：

#### 方案C: 自定义GIF播放器 (中级难度)
- 直接使用gifdec库
- 手动管理帧缓冲
- 通过LVGL canvas绘制
- 优点：更精细的控制
- 缺点：开发工作量大

#### 方案D: 视频格式 (高级难度)
- 使用MP4/WebM格式
- 需要视频解码库
- 优点：更高压缩率
- 缺点：复杂度高，内存需求大

### 16.7 最终建议

**✅ 推荐使用方案B (LVGL内置GIF)**

**理由**:
1. 与现有技术栈完全兼容
2. 实施成本低（1-2天即可完成）
3. 维护简单，官方支持
4. 性能足够满足UI状态指示需求
5. 不需要改变硬件或驱动层

**不推荐derdacavga/Gif-Player的原因**:
1. ❌ 技术栈不匹配（Arduino vs ESP-IDF）
2. ❌ 需要大量移植工作（估计2周+）
3. ❌ 与LVGL集成困难
4. ❌ 硬件驱动需要重写
5. ❌ 维护成本高，偏离项目主线

### 16.8 实施计划

**阶段1: 基础启用 (0.5天)**
- [ ] 启用CONFIG_LV_USE_GIF
- [ ] 测试基本GIF播放功能
- [ ] 验证SD卡读取

**阶段2: 状态管理器 (1天)**
- [ ] 实现ui_state_gif模块
- [ ] 集成到app_state状态机
- [ ] 添加内存管理

**阶段3: GIF资源准备 (0.5天)**
- [ ] 设计/获取6个状态GIF
- [ ] 优化GIF参数（尺寸、帧率、颜色）
- [ ] 上传到SD卡测试

**阶段4: 性能调优 (0.5天)**
- [ ] 监控内存使用
- [ ] 调整帧率和缓存
- [ ] 测试长时间运行稳定性

**总预计时间**: 2-3天

### 16.9 相关文件
- `managed_components/lvgl__lvgl/src/extra/libs/gif/`: LVGL GIF模块源码
- `sdkconfig`: LVGL配置文件
- `components/ui/`: UI组件目录（新增ui_state_gif）
- `main/app_state.c`: 状态机集成点

## 17. OpenClaw协议版本兼容性修复 (2026-05-16)

### 17.1 问题现象
ESP32设备连接OpenClaw服务器时反复失败，日志显示：
```
E openclaw: Request error [INVALID_REQUEST]: protocol mismatch
W openclaw: WS CLOSE frame: code=1002
[ws] protocol mismatch conn=... client=cli cli v0.5.0
```

### 17.2 根本原因分析

#### 1. 协议版本检查机制
OpenClaw服务器使用双边区间匹配策略验证客户端协议版本：
```typescript
const { minProtocol, maxProtocol } = connectParams;
if (maxProtocol < PROTOCOL_VERSION || minProtocol > PROTOCOL_VERSION) {
    markHandshakeFailure("protocol-mismatch");
    close(1002, "protocol mismatch");
}
```

**关键点**：服务器的`PROTOCOL_VERSION`必须落在客户端声明的`[minProtocol, maxProtocol]`范围内。

#### 2. 客户端元数据字段错误
初始实现中存在多个与协议规范不符的字段：

| 字段 | 错误值 | 正确值 | 说明 |
|------|--------|--------|------|
| `client.id` | `"gateway-client"` | `"cli"` | CLI客户端标准ID |
| `client.version` | `"0.2.0"` | `"0.5.0"` | 需与服务器期望版本对齐 |
| `client.mode` | `"operator"` → `"cli"` | `"cli"` | CLI客户端mode应为"cli" |
| `locale` | 缺失 | `"en-US"` | 协议必需字段 |
| `userAgent` | 缺失 | `"HeyClawy/0.5.0"` | 协议必需字段 |

#### 3. Auth Payload格式不一致
认证payload中的clientId字段与client对象不匹配：
```
# 错误
v2|device_id|gateway-client|operator|operator|...

# 正确
v2|device_id|cli|cli|operator|...
```

### 17.3 详细解决方案

#### 修改1: 更新客户端元数据字段
**文件**: `components/openclaw/openclaw_client.c` (第361-369行)

```c
cJSON *client = cJSON_AddObjectToObject(params, "client");
cJSON_AddStringToObject(client, "id", "cli");  // 修正为"cli"
cJSON_AddStringToObject(client, "version", "0.5.0");  // 升级到0.5.0
#if CONFIG_IDF_TARGET_ESP32S3
    cJSON_AddStringToObject(client, "platform", "esp32s3");
#else
    cJSON_AddStringToObject(client, "platform", "esp32");
#endif
cJSON_AddStringToObject(client, "mode", "cli");  // CLI客户端使用"cli"
```

#### 修改2: 添加必需的locale和userAgent字段
**文件**: `components/openclaw/openclaw_client.c` (第381-390行)

```c
/* Request tool event broadcasting and proactive notifications */
cJSON *caps = cJSON_AddArrayToObject(params, "caps");
cJSON_AddItemToArray(caps, cJSON_CreateString("tool-events"));
cJSON_AddItemToArray(caps, cJSON_CreateString("proactive"));

/* Add locale and userAgent fields per protocol spec */
cJSON_AddStringToObject(params, "locale", "en-US");

char user_agent[64];
snprintf(user_agent, sizeof(user_agent), "HeyClawy/0.5.0");
cJSON_AddStringToObject(params, "userAgent", user_agent);
```

#### 修改3: 扩展协议版本范围
**文件**: `components/openclaw/openclaw_client.c` (第357-359行)

```c
cJSON *params = cJSON_AddObjectToObject(root, "params");
cJSON_AddNumberToObject(params, "minProtocol", 3);
cJSON_AddNumberToObject(params, "maxProtocol", 4);  // 支持协议版本3-4
```

**理由**：使用区间`[3, 4]`而非固定值`3`，可以兼容不同版本的OpenClaw服务器，避免因服务器升级导致的连接失败。

#### 修改4: 同步Auth Payload格式
**文件**: `components/openclaw/openclaw_client.c` (第408-410行)

```c
snprintf(auth_payload, sizeof(auth_payload),
         "v2|%s|cli|cli|operator|%s|%" PRId64 "|%s|%s",
         s_oc.device_id, scopes_joined, epoch_ms, auth_token, s_oc.nonce);
```

**关键变更**：将第二个字段从`"gateway-client"`改为`"cli"`，第三个字段从`"operator"`改为`"cli"`，与client对象保持一致。

#### 修改5: 增强调试日志
**文件**: `components/openclaw/openclaw_client.c` (第435-438行)

```c
char *json_str = cJSON_PrintUnformatted(root);
if (json_str) {
    int jlen = strlen(json_str);
    ESP_LOGI(TAG, "Connect req (%d bytes): %s", jlen, json_str);  // 打印完整JSON
    esp_websocket_client_send_text(s_oc.ws, json_str, jlen, pdMS_TO_TICKKS(5000));
    ESP_LOGI(TAG, "Sent connect request with device authentication");
    free(json_str);
}
```

**目的**：输出完整的connect请求JSON，便于远程诊断协议字段是否正确。

### 17.4 技术要点总结

#### 1. 协议版本区间设计
- **双边区间匹配**：客户端声明`[minProtocol, maxProtocol]`，服务器检查自身版本是否在此范围内
- **向前兼容**：设置`maxProtocol`高于当前版本，可兼容未来服务器升级
- **向后兼容**：设置`minProtocol`低于当前版本，可兼容旧版服务器

#### 2. 客户端标识规范
根据OpenClaw协议文档，不同客户端类型的标识规则：

| 客户端类型 | client.id | client.mode | role | 说明 |
|-----------|-----------|-------------|------|------|
| CLI客户端 | `"cli"` | `"cli"` | `"operator"` | 命令行工具、嵌入式设备 |
| Web UI | `"web-ui"` | `"web"` | `"operator"` | 浏览器控制界面 |
| iOS节点 | `"ios-node"` | `"node"` | `"node"` | iOS设备能力宿主 |
| macOS应用 | `"macos-app"` | `"operator"` | `"operator"` | macOS桌面应用 |

**注意**：`client.mode`的值取决于客户端类型，CLI客户端应使用`"cli"`而非`"operator"`。

#### 3. Auth Payload格式规范
V2版本的auth payload格式（管道符分隔）：
```
v2|deviceId|clientId|clientMode|role|scopes|signedAtMs|token|nonce
```

**字段对应关系**：
- `clientId` 必须与 `params.client.id` 一致
- `clientMode` 必须与 `params.client.mode` 一致
- `role` 必须与 `params.role` 一致
- `scopes` 必须与 `params.scopes` 数组内容一致（逗号分隔）

#### 4. 必需字段清单
根据协议规范，connect请求的`params`对象必须包含：

**核心字段**：
- ✅ `minProtocol` / `maxProtocol`：协议版本区间
- ✅ `client`：客户端元数据对象
- ✅ `role`：角色（`"operator"`或`"node"`）
- ✅ `scopes`：作用域数组（operator角色必需）
- ✅ `auth`：认证信息对象
- ✅ `device`：设备身份对象（非本地连接必需）

**推荐字段**：
- ✅ `locale`：语言区域（如`"en-US"`）
- ✅ `userAgent`：用户代理字符串
- ✅ `caps`：能力声明数组

### 17.5 教训与最佳实践

#### 1. 协议兼容性原则
- **使用版本区间而非固定值**：`[3, 4]`比`3`更具弹性
- **严格遵循协议文档**：所有字段名、枚举值必须与官方规范一致
- **保持字段一致性**：auth payload中的标识必须与JSON对象对应字段完全一致

#### 2. 调试策略
- **打印完整请求JSON**：便于对比协议规范和实际发送内容
- **记录服务器响应细节**：包括错误码、错误消息、关闭原因
- **对比成功连接日志**：参考官方客户端的连接参数

#### 3. 防御性编程
- **UTF-8验证**：所有网络传输的字符串必须验证UTF-8有效性
- **字段完整性检查**：发送前确认所有必需字段已填充
- **版本协商日志**：记录协商后的协议版本，便于排查兼容性问题

#### 4. 文档同步
- **及时更新协议文档**：当发现协议细节变化时，立即更新HARNESS.md
- **记录已知陷阱**：如`client.mode`的正确取值、auth payload格式等
- **提供参考链接**：附上官方协议文档URL，方便后续查阅

### 17.6 验证步骤

1. **编译并烧录固件**：
   ```bash
   ./build_audio_board.sh flash monitor
   ```

2. **观察连接日志**，确认以下信息：
   - ✅ Connect请求包含正确的字段：`"client":{"id":"cli","mode":"cli","version":"0.5.0"}`
   - ✅ 协议版本区间：`"minProtocol":3,"maxProtocol":4`
   - ✅ 包含必需字段：`"locale":"en-US"`, `"userAgent":"HeyClawy/0.5.0"`
   - ✅ Auth payload格式正确：`v2|device_id|cli|cli|operator|...`

3. **确认连接成功**：
   - 收到`hello-ok`响应
   - 状态切换到`OPENCLAW_STATE_CONNECTED`
   - 无`protocol mismatch`错误

4. **功能测试**：
   - 发送聊天消息并接收响应
   - 测试设备控制命令
   - 长时间运行稳定性测试（>30分钟）

### 17.7 相关文件
- `components/openclaw/openclaw_client.c`: OpenClaw WebSocket客户端实现
- `components/openclaw/include/openclaw_client.h`: 客户端API头文件
- `PRD/Project_Resources/openclaw-api.md`: OpenClaw Gateway API文档
- `https://github.com/openclaw/openclaw/blob/main/docs/gateway/protocol.md`: 官方协议规范

### 17.8 参考资料
- **OpenClaw协议文档**: https://github.com/openclaw/openclaw/blob/main/docs/gateway/protocol.md
- **协议版本检查逻辑**: https://github.com/openclaw/openclaw/blob/main/src/gateway/server/ws-connection/message-handler.ts
- **HeyClawy参考实现**: `/home/ubuntu/esp32/HeyClawy/components/openclaw/`

## 18. 系统稳定性优化 - GIF内存清理与重连死机修复 (2026-05-16)

### 18.1 问题现象

#### 崩溃1: Interrupt wdt timeout on CPU0
```
Guru Meditation Error: Core  0 panic'ed (Interrupt wdt timeout on CPU0).
Backtrace: 0x4037f676:0x3fcddae0 0x40384591:0x3fcddb00 ...
```
**发生时机**：OpenClaw断开后重连过程中

#### 崩溃2: LoadProhibited异常
```
Guru Meditation Error: Core  1 panic'ed (LoadProhibited). Exception was unhandled.
EXCVADDR: 0x00000014  (访问了无效地址0x14)
Backtrace: 0x40384fb3:0x3fcbf960 0x403853b9:0x3fcbf980 ...
```
**发生时机**：TTS播放结束后，状态从SPEAKING切换到IDLE时

#### STT识别失败
```
I (119331) stt: Task finished. Result: ''
W (119335) voice_chat: STT failed despite detected speech, discarding
```
录音17.3秒但STT返回空结果

### 18.2 根本原因分析

#### 1. GIF对象use-after-free导致LoadProhibited异常

**问题代码** (`ui_state_gif.c`):
```c
/* 错误的删除顺序 */
lv_obj_del(s_current_gif);  // 删除对象
s_current_gif = NULL;       // 然后才清空指针
```

**问题分析**：
- LVGL的`lv_obj_del()`是异步操作，不会立即释放所有资源
- GIF对象内部有定时器回调，可能在删除后仍然被触发
- 如果全局指针在删除后才设置为NULL，定时器回调可能访问已释放的内存
- `EXCVADDR: 0x00000014` 表明尝试访问结构体偏移量0x14的成员，但基指针为NULL或野指针

**正确做法**：
```c
/* 先清空全局指针，再删除对象 */
lv_obj_t *old_gif = s_current_gif;
s_current_gif = NULL;           // 立即清空全局指针
s_current_state = GIF_STATE_COUNT;
lv_obj_del(old_gif);            // 使用局部变量删除
lv_refr_now(NULL);              // 强制刷新
lv_task_handler();              // 处理待处理任务
```

#### 2. OpenClaw重连时资源清理不彻底

**问题代码** (`openclaw_client.c`):
```c
if (s_oc.ws) {
    esp_websocket_client_stop(s_oc.ws);
    esp_websocket_client_destroy(s_oc.ws);  // 立即销毁
    s_oc.ws = NULL;
}
```

**问题分析**：
- `esp_websocket_client_stop()`是异步操作，需要时间完成
- 立即调用`destroy()`可能导致底层TCP连接未完全关闭
- 多次快速重连时，旧连接的socket资源未释放，导致新连接失败
- "Connection reset by peer"错误表明服务器拒绝了连接（可能是端口占用）

#### 3. STT空结果缺少诊断信息

**问题**：
- STT返回空字符串时没有记录音频统计信息
- 无法判断是音频质量问题、语言不匹配还是网络问题
- 缺少调试信息，难以定位根因

### 18.3 详细解决方案

#### 修复1: GIF对象安全删除机制

**文件**: `components/ui/ui_state_gif.c`

**修改点1**: `ui_state_gif_show_for_state()` - 添加空指针检查
```c
void ui_state_gif_show_for_state(ui_state_t state)
{
    gif_state_type_t gif_state = ui_state_to_gif_state(state);
    
    /* Only update if state changed */
    if (gif_state == s_current_state && s_current_gif) {
        return;
    }
    
    /* If new state has no GIF configured, just hide current one */
    const char *filepath = gif_filenames[gif_state];
    if (!filepath) {
        ESP_LOGW(TAG, "No GIF configured for state %d, hiding", gif_state);
        ui_state_gif_hide();
        return;
    }
    
    ui_state_gif_show(gif_state);
}
```

**修改点2**: `ui_state_gif_show()` - 先清空指针再删除
```c
/* Hide and delete current GIF if exists */
if (s_current_gif) {
    ESP_LOGD(TAG, "Deleting previous GIF object");
    
    /* Store pointer to local variable and clear global immediately */
    lv_obj_t *old_gif = s_current_gif;
    s_current_gif = NULL;              // 立即清空全局指针
    s_current_state = GIF_STATE_COUNT;
    
    /* Delete the GIF object - this frees LVGL resources */
    lv_obj_del(old_gif);               // 使用局部变量删除
    
    /* Force immediate LVGL refresh to clean up */
    lv_refr_now(NULL);
    
    /* Additional memory cleanup */
    lv_task_handler();
    
    ESP_LOGD(TAG, "Previous GIF deleted, running garbage collection");
}
```

**修改点3**: `ui_state_gif_hide()` - 同样的安全删除模式
```c
void ui_state_gif_hide(void)
{
    if (!lvgl_port_lock(500)) {
        return;
    }
    
    if (s_current_gif) {
        /* Clear global pointer BEFORE deleting to prevent use-after-free */
        lv_obj_t *old_gif = s_current_gif;
        s_current_gif = NULL;
        s_current_state = GIF_STATE_COUNT;
        
        /* Delete the GIF object */
        lv_obj_del(old_gif);
        
        /* Force refresh to ensure cleanup */
        lv_refr_now(NULL);
        lv_task_handler();
        
        ESP_LOGI(TAG, "GIF hidden and cleaned up");
    }
    
    lvgl_port_unlock();
}
```

#### 修复2: OpenClaw重连资源清理增强

**文件**: `components/openclaw/openclaw_client.c`

**修改**: 在停止和销毁之间添加延迟
```c
if (s_oc.ws) {
    ESP_LOGD(TAG, "Destroying existing WebSocket client before reconnect");
    /* Stop only if it was actually started to avoid warning */
    if (s_oc.state != OPENCLAW_STATE_DISCONNECTED && s_oc.state != OPENCLAW_STATE_ERROR) {
        esp_websocket_client_stop(s_oc.ws);
        /* Give the stop operation time to complete */
        vTaskDelay(pdMS_TO_TCKS(100));  // 等待100ms让stop完成
    }
    esp_websocket_client_destroy(s_oc.ws);
    s_oc.ws = NULL;
    /* Small delay to allow resources to be freed */
    vTaskDelay(pdMS_TO_TCKS(50));      // 再等待50ms让资源释放
}
```

**原理**：
- `esp_websocket_client_stop()`会关闭TCP连接并清理任务
- 这个操作是异步的，需要时间完成
- 添加100ms延迟确保stop操作完成
- destroy后再延迟50ms确保socket资源完全释放
- 避免快速重连时的资源竞争

#### 修复3: STT空结果诊断增强

**文件**: `components/stt/stt_client.c`

**修改**: 在task-finished事件中添加详细日志
```c
} else if (strcmp(event_str, "task-finished") == 0 || strcmp(event_str, "task-failed") == 0) {
    if (strcmp(event_str, "task-failed") == 0) {
        const char *err_msg = cJSON_GetObjectItem(header, "error_message") ? 
                              cJSON_GetObjectItem(header, "error_message")->valuestring : "Unknown";
        const char *err_code = cJSON_GetObjectItem(header, "error_code") ? 
                               cJSON_GetObjectItem(header, "error_code")->valuestring : "N/A";
        ESP_LOGE(TAG, "Task failed: %s (code=%s)", err_msg, err_code);
    } else {
        /* Log detailed info about the finished task */
        int text_len = strlen(s_stt.final_text);
        ESP_LOGI(TAG, "Task finished. Result: '%s' (len=%d)", s_stt.final_text, text_len);
        
        /* If result is empty, log a warning with context */
        if (text_len == 0) {
            ESP_LOGW(TAG, "WARNING: STT returned empty result despite audio being sent");
            ESP_LOGW(TAG, "This could be due to:");
            ESP_LOGW(TAG, "  1. Audio quality issues (too quiet, too noisy)");
            ESP_LOGW(TAG, "  2. Language mismatch (speaking Chinese but model expects English)");
            ESP_LOGW(TAG, "  3. Network timeout during processing");
            ESP_LOGW(TAG, "  4. Server-side recognition failure");
        }
    }
    s_stt.state = STT_STATE_IDLE;
    xSemaphoreGive(s_stt.result_sem);
}
```

### 18.4 技术要点总结

#### 1. LVGL对象生命周期管理

**关键原则**：
- **先清空引用，再删除对象**：防止其他代码在删除过程中访问对象
- **使用局部变量保存指针**：避免全局指针被意外修改
- **强制刷新**：`lv_refr_now()` + `lv_task_handler()` 确保清理完成
- **加锁保护**：所有LVGL操作必须在`lvgl_port_lock/unlock`之间

**常见陷阱**：
```c
// ❌ 错误：删除后仍有回调访问全局指针
lv_obj_del(obj);
global_ptr = NULL;  // 太晚了！

// ✅ 正确：先清空再删除
lv_obj_t *temp = obj;
global_ptr = NULL;  // 立即清空
lv_obj_del(temp);   // 使用临时变量
```

#### 2. ESP-IDF资源清理时序

**WebSocket客户端销毁流程**：
1. `esp_websocket_client_stop()` - 异步停止任务，关闭TCP连接
2. **等待** - 给底层操作时间完成（100ms）
3. `esp_websocket_client_destroy()` - 释放内存和句柄
4. **等待** - 确保socket资源完全释放（50ms）
5. 设置指针为NULL

**为什么需要延迟**：
- ESP-IDF的网络操作大多是异步的
- TCP连接有TIME_WAIT状态，需要时间清理
- 立即销毁可能导致资源泄漏或端口占用

#### 3. 调试策略

**对于LoadProhibited异常**：
1. 查看`EXCVADDR`值：
   - `0x00000000` - NULL指针解引用
   - `0x000000XX` - 小偏移量，通常是结构体成员访问
   - `0xFFFFFFFF` - 野指针或已释放内存
2. 分析backtrace找到出错的函数
3. 检查该函数访问的所有指针是否有效
4. 添加防御性检查（NULL判断）

**对于中断看门狗超时**：
1. 检查是否有长时间运行的中断处理程序
2. 确认不在中断上下文中调用阻塞API（如vTaskDelay）
3. 检查是否有死锁或优先级反转
4. 增加详细的日志定位卡住的位置

### 18.5 教训与最佳实践

#### 1. LVGL编程规范
- **永远不要在删除对象后立即访问它**
- **删除前必须先清空所有引用**
- **使用`lv_refr_now()`和`lv_task_handler()`确保清理**
- **所有LVGL操作必须加锁**

#### 2. 网络资源管理规范
- **异步操作后必须等待完成**
- **销毁前先停止，停止后等待，再销毁**
- **添加合理的延迟（50-100ms）**
- **记录详细的连接/断开日志**

#### 3. 防御性编程
- **每次访问指针前检查NULL**
- **删除对象后立即将指针设为NULL**
- **使用局部变量保存临时指针**
- **添加详细的错误诊断日志**

#### 4. 内存管理
- **PSRAM用于大缓冲区，内部RAM用于关键任务**
- **及时释放不再需要的资源**
- **监控堆内存使用情况**
- **避免频繁的malloc/free**

### 18.6 验证步骤

1. **编译并烧录固件**：
   ```bash
   ./build_audio_board.sh flash monitor
   ```

2. **测试GIF切换**：
   - 观察状态切换时是否有崩溃
   - 特别关注TTS结束后的IDLE状态切换
   - 连续切换状态10次以上，确保稳定性

3. **测试OpenClaw重连**：
   - 手动断开WiFi，观察重连过程
   - 确认无"Interrupt wdt timeout"错误
   - 确认重连成功且无资源泄漏

4. **测试STT识别**：
   - 进行语音对话，观察STT结果
   - 如果出现空结果，检查日志中的诊断信息
   - 根据日志提示调整音频质量或模型配置

5. **长时间运行测试**：
   - 连续运行30分钟以上
   - 监控内存使用情况
   - 确认无崩溃、无内存泄漏

### 18.7 相关文件
- `components/ui/ui_state_gif.c`: GIF对象管理，修复use-after-free
- `components/openclaw/openclaw_client.c`: WebSocket重连，增强资源清理
- `components/stt/stt_client.c`: STT客户端，增强空结果诊断
- `main/app_tasks.c`: 重连看门狗，监控连接状态

### 18.8 相关HARNESS章节
- **第9节**: WebSocket断开(code=1007)和内存分配失败修复
- **第13.1节**: 唤醒词检测后内存分配失败导致重启修复
- **第13.2节**: LVGL暂停优化和SD卡文件名乱码修复
- **第17节**: OpenClaw协议版本兼容性修复

## 19. MP3播放后无法休眠和唤醒问题修复 (2026-05-16)

### 19.1 问题现象

用户报告：播放完MP3歌曲后，设备出现以下异常行为：
1. ❌ 一直停留在IDLE状态，无法自动进入休眠
2. ❌ 说出唤醒词后无响应，无法通过语音唤醒
3. ❌ 只能通过物理按键（BOOT键）才能唤醒
4. ❌ 唤醒词检测日志显示RMS值正常，但系统不响应

### 19.2 日志分析

从日志中提取的关键时间线：

```
I (76401) ui_main: State transition: SPEAKING → IDLE
I (76652) app_tasks: Auto-listen: continuing conversation  ← 连续对话模式激活
I (77464) aiclaw: Wake word detected!                       ← 唤醒词正常工作
...
I (108163) mp3_player: Playing: file://sdcard/伍佰 & China Blue - 挪威的森林.mp3
...
I (504662) aiclaw: MP3 playback finished                    ← MP3播放结束
I (511879) wake_word: Audio monitor: RMS=257                ← 唤醒词检测工作
I (526279) wake_word: Audio monitor: RMS=37
...
I (641479) wake_word: Audio monitor: RMS=65                  ← 持续检测到声音但无响应
```

**关键发现**：
- 唤醒词检测模块正常工作（有RMS输出）
- 但系统没有响应唤醒词事件
- MP3播放结束后没有任何状态切换日志

### 19.3 根本原因分析

#### 原因1: 连续对话标志未重置

**代码路径**：
1. TTS播放AI回复 → `app_on_chat_response()` 设置 `g_continue_listening = true`
2. TTS结束 → 触发自动监听（第76652ms日志）
3. 用户说话 → AI回复并执行MP3播放命令
4. **MP3播放结束** → `on_mp3_complete()` 回调被调用
5. ❌ **但没有重置`g_continue_listening`标志**

**后果**：
- 系统认为仍在"连续对话模式"中
- 不会触发表达式超时检查
- 无法进入休眠状态

#### 原因2: UI状态未正确切换

**问题流程**：
```c
// MP3播放开始时
mp3_player_play("song.mp3");  // ✅ 开始播放
// ❌ 没有调用 app_set_state(UI_STATE_PLAYING_MP3)

// MP3播放结束时
on_mp3_complete();             // ✅ 回调被调用
// ❌ 没有调用 app_set_state(UI_STATE_IDLE)
```

**后果**：
- MP3播放期间，UI状态可能是RESPONSE或其他状态
- 播放结束后，状态没有回到IDLE
- GIF动画可能显示错误的内容

#### 原因3: 唤醒词响应条件过于严格

**原有代码** (`app_main.c` 第679行):
```c
if (cur == UI_STATE_IDLE || cur == UI_STATE_RESPONSE) {
    // 只有这两个状态才响应唤醒词
    voice_chat_start();
}
```

**问题**：
- 如果当前状态是`UI_STATE_PLAYING_MP3`，唤醒词会被忽略
- 即使MP3播放已结束，如果状态没有切换到IDLE，唤醒词也无法工作

### 19.4 详细解决方案

#### 修复1: MP3播放完成后重置连续对话标志

**文件**: `main/app_main.c`

```c
static void on_mp3_complete(void)
{
    ESP_LOGI(TAG, "MP3 playback finished");
    
    /* CRITICAL: Reset continuous conversation mode after MP3 playback.
     * MP3 playback is a deliberate user action that should break the
     * continuous conversation flow. If we don't reset this flag, the
     * system will stay in IDLE forever waiting for auto-listen, but
     * wake word detection might be disabled or in wrong state. */
    extern bool g_continue_listening;
    if (g_continue_listening) {
        ESP_LOGI(TAG, "Resetting continuous conversation mode after MP3");
        g_continue_listening = false;
    }
    
    /* CRITICAL: Switch UI state back to IDLE so wake word can trigger voice chat.
     * Without this, the system stays in PLAYING_MP3 state and wake word events
     * are ignored because app_main.c only responds in IDLE or RESPONSE states. */
    app_set_state(UI_STATE_IDLE);
    
    /* UI update is handled inside mp3_player event callback via state_cb */
}
```

**关键点**：
- 使用`extern`声明访问全局变量`g_continue_listening`
- 在重置前检查标志是否为true，避免不必要的日志
- 调用`app_set_state(UI_STATE_IDLE)`确保状态正确切换
- 这会触发GIF动画切换到idle.gif

#### 修复2: MP3播放开始时设置正确状态

**文件**: `main/app_state.c`

在两个MP3播放入口点添加状态设置：

```c
// 位置1: play:filename 命令处理（第193-200行）
if (strncmp(val, "play:", 5) == 0) {
    esp_err_t ret = mp3_player_play(val + 5);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MP3 play: %s", val + 5);
        /* Set UI state to PLAYING_MP3 so wake word detection is properly handled */
        app_set_state(UI_STATE_PLAYING_MP3);
    } else if (ret == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "MP3 file not found: %s", val + 5);
    } else {
        ESP_LOGE(TAG, "MP3 play failed: %s (err=%d)", val + 5, ret);
    }
}

// 位置2: index:N 命令处理（第217-228行）
} else if (strncmp(val, "index:", 6) == 0) {
    int idx = atoi(val + 6) - 1;
    if (idx >= 0 && idx < s_sd_mp3_count) {
        esp_err_t ret = mp3_player_play(s_sd_mp3_names[idx]);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "MP3 play index %d: %s", idx + 1, s_sd_mp3_names[idx]);
            /* Set UI state to PLAYING_MP3 so wake word detection is properly handled */
            app_set_state(UI_STATE_PLAYING_MP3);
        } else {
            ESP_LOGE(TAG, "MP3 play index %d failed: err=%d", idx + 1, ret);
        }
    }
}
```

**效果**：
- MP3播放时，UI状态明确为`UI_STATE_PLAYING_MP3`
- GIF动画会显示speaking.gif（根据ui_state_gif.c的映射）
- 便于调试和状态追踪

#### 修复3: 允许从PLAYING_MP3状态响应唤醒词

**文件**: `main/app_main.c`

```c
if (ev & WAKE_WORD_BIT) {
    app_reset_activity_timer();
    ESP_LOGI(TAG, "Wake word detected!");
    ui_state_t cur = ui_get_state();
    
    /* Allow wake word from IDLE, RESPONSE, or PLAYING_MP3 states.
     * MP3 playback should be interruptible by wake word for voice control. */
    if (cur == UI_STATE_IDLE || cur == UI_STATE_RESPONSE || cur == UI_STATE_PLAYING_MP3) {
        /* CRITICAL: Stop TTS immediately to free internal RAM for STT */
        if (tts_is_playing()) {
            ESP_LOGI(TAG, "Stopping TTS due to wake word");
            tts_stop();
            vTaskDelay(pdMS_TO_TCKS(50));
        }
        
        /* If in MP3 playback mode, stop it first */
        if (cur == UI_STATE_PLAYING_MP3) {
            ESP_LOGI(TAG, "Stopping MP3 playback due to wake word");
            mp3_player_stop();
            vTaskDelay(pdMS_TO_TCKS(100));  // 等待MP3停止完成
        }
        
        wake_word_pause();
        voice_chat_start();
        wake_word_resume();
    }
}
```

**改进点**：
1. **扩展状态检查**：增加`UI_STATE_PLAYING_MP3`到允许响应的状态列表
2. **主动停止MP3**：在启动语音聊天前先停止MP3播放
3. **添加延迟**：给MP3播放器100ms时间清理资源
4. **详细日志**：记录每个操作步骤，便于调试

### 19.5 技术要点总结

#### 1. 连续对话模式的生命周期管理

**设计原则**：
- 连续对话模式(`g_continue_listening`)应该在特定边界条件下重置
- MP3播放是一个明确的"打断点"，应该终止连续对话
- 其他打断点包括：用户取消、长时间静默、手动按键等

**正确的重置时机**：
```c
// ✅ MP3播放完成
on_mp3_complete() { g_continue_listening = false; }

// ✅ 用户取消录音
voice_chat.c: g_continue_listening = false;

// ✅ 长时间无语音
voice_chat.c: g_continue_listening = false;

// ✅ TTS播放失败
app_tasks.c: g_continue_listening = false;
```

#### 2. UI状态机的完整性

**状态切换规则**：
- **进入操作时**：立即设置对应状态
- **退出操作时**：立即恢复到上一个状态或IDLE
- **异常情况下**：也要确保状态恢复（如MP3播放失败）

**MP3播放的状态流**：
```
IDLE/RESPONSE 
    ↓ [DEVICE:mp3=...]
PLAYING_MP3  ← app_set_state() 在这里
    ↓ MP3播放完成
IDLE         ← on_mp3_complete() 在这里
```

#### 3. 唤醒词响应的状态白名单

**设计理念**：
- 不是所有状态都应该响应唤醒词
- 需要明确定义哪些状态可以被打断
- 不同状态的打断处理方式可能不同

**推荐的状态白名单**：
```c
// 可以响应唤醒词的状态
- UI_STATE_IDLE          // 空闲状态，随时可唤醒
- UI_STATE_RESPONSE      // 显示回复，可打断
- UI_STATE_PLAYING_MP3   // 播放音乐，可打断

// 不应该响应唤醒词的状态
- UI_STATE_LISTENING     // 正在录音，避免冲突
- UI_STATE_THINKING      // AI思考中，等待结果
- UI_STATE_STREAMING     // 流式接收，避免中断
- UI_STATE_TTS_PLAYING   // TTS播放中（可选，看需求）
```

#### 4. 资源清理时序

**MP3停止的正确流程**：
```c
mp3_player_stop();           // 1. 请求停止
vTaskDelay(pdMS_TO_TCKS(100)); // 2. 等待清理完成
voice_chat_start();          // 3. 启动新任务
```

**为什么需要延迟**：
- `mp3_player_stop()`是异步操作
- 底层音频解码器需要时间释放资源
- I2S通道需要重新配置
- 立即启动新任务可能导致资源竞争

### 19.6 教训与最佳实践

#### 1. 状态管理的完整性
- **每次状态变化都要成对出现**：进入时设置，退出时恢复
- **不要依赖隐式状态转换**：显式调用`app_set_state()`
- **异常路径也要处理**：失败时也要恢复状态

#### 2. 全局标志的生命周期
- **明确标志的设置点和清除点**
- **在边界条件下重置标志**（如模式切换、任务完成）
- **添加日志跟踪标志变化**

#### 3. 唤醒词处理的灵活性
- **支持从多个状态响应唤醒词**
- **不同状态可能需要不同的预处理**（如停止MP3）
- **添加足够的延迟让资源清理完成**

#### 4. 调试策略
- **记录完整的状态转换日志**
- **监控全局标志的变化**
- **在关键路径添加诊断日志**

### 19.7 验证步骤

1. **编译并烧录固件**：
   ```bash
   ./build_audio_board.sh flash monitor
   ```

2. **测试MP3播放和唤醒**：
   - 说"播放一首歌"，确认MP3开始播放
   - 观察UI状态是否为PLAYING_MP3
   - 在播放过程中说"你好小智"
   - 确认MP3被停止，语音聊天启动
   - 观察日志中有"Stopping MP3 playback due to wake word"

3. **测试MP3播放后的休眠**：
   - 让MP3自然播放完毕
   - 确认日志中有"Resetting continuous conversation mode after MP3"
   - 确认状态切换到IDLE
   - 等待60秒，确认进入SLEEP状态（息屏）

4. **测试唤醒词响应**：
   - 在IDLE状态下说"你好小智"
   - 确认识别成功并进入LISTENING状态
   - 完成一次完整的语音对话

5. **长时间运行测试**：
   - 多次播放MP3并唤醒
   - 确认无内存泄漏
   - 确认状态转换始终正确

### 19.8 相关文件
- `main/app_main.c`: MP3完成回调和唤醒词处理
- `main/app_state.c`: MP3命令处理和状态设置
- `components/mp3_player/mp3_player.c`: MP3播放器实现
- `components/ui/ui_state_gif.c`: GIF状态映射

### 19.9 相关HARNESS章节
- **第8节**: 状态机与交互超时约束
- **第13.1节**: 唤醒词检测后内存分配失败导致重启修复
- **第18节**: 系统稳定性优化 - GIF内存清理与重连死机修复