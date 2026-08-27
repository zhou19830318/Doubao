# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概览

- **项目**：基于 Waveshare ESP32-S3-Touch-AMOLED-2.06 的豆包 AI 语音对话机器人（桌面形态）。
- **技术栈**：ESP-IDF v5.5.5 + LVGL 9.5.0（esp_lvgl_port 2.6.0）+ esp_websocket_client 1.1.0 + esp_codec_dev 1.5.0 + esp-sr（唤醒词）。目标 esp32s3，32MB flash / 8MB PSRAM。
- **设计文档**：`docs/superpowers/specs/2026-08-16-doubao-voice-robot-design.md`（架构、音频链路、协议附录、状态图、里程碑——所有设计决策以它为准，改动设计先改文档）。
- **硬件资料**：`PRD/Project_Resources/` 目录（豆包 API 协议 PDF/MD + 旧项目经验文档，共 21 篇）。
- **代码骨架来源**：`/home/conor/esp/esp32/AIWatch_Ver2.0/`（同板卡已验证的成熟工程）。按设计文档"搬骨架换心脏"：板级驱动、wifi_manager、settings、webserver、wake_word、mp3_player、notes_manager、error_log、状态机直接搬；stt/tts/openclaw 三件套替换为新的 `doubao_voice` 组件；UI 重做为气泡列表。

## 构建与烧录

```bash
# 首次（或在 build/ 出现怪异行为后）：idf.py fullclean
idf.py set-target esp32s3          # 若尚未设置目标
idf.py menuconfig                  # 板卡选择在 "Doubao Application Configuration" 下
idf.py build
idf.py -p /dev/ttyACM0 -b 921600 flash   # Linux 原生 USB-JTAG 口；开发期用 app-flash 更快
idf.py -p /dev/ttyACM0 monitor     # Ctrl+] 退出；自动解码 panic 地址为 文件:行号
# 串口监控注意：CH342 芯片必须 rts=False，否则 bootloader 后串口阻塞（PRD serial-monitoring.md）
```

- 分区表（沿用 Ver2.0）：nvs 24K / phy_init 4K / **factory app 8MB** / model SPIFFS 960K（WakeNet 模型 ~290KB）/ storage SPIFFS 2M。
- sdkconfig 关键项沿用 Ver2.0（sdkconfig.defaults）：PSRAM Octal 80M、SPIRAM_USE_MALLOC + ALWAYSINTERNAL=256、指令/只读数据进 PSRAM、WiFi/LWIP 缓冲进 PSRAM、MALLOC_RESERVE_INTERNAL=64KB、LWIP_MAX_SOCKETS=16、FATFS codepage 936 + UTF-8、mbedtls 动态缓冲 + 关闭硬件 AES。
- 凭据/密钥：禁止硬编码在固件；走 settings NVS + webserver 输入（设计文档 §5.6）。

## 架构约束

- **依赖方向**：main → components（main 自动依赖所有组件）；**组件禁止依赖 main**（循环依赖），跨层通信走事件组注入（setter 模式：main 调 `ui_set_event_group()` 注入）。
- **跨任务通信**：FreeRTOS Event Group（`g_app_events` 事件位）+ 回调注册；UI 线程安全：非 LVGL 任务调 UI 必须 `lvgl_port_lock()`/`unlock()`；**持锁任务禁止 vTaskSuspend/vTaskDelete**（递归锁死锁）。
- **board 组件是唯一碰硬件引脚的组件**：新增板卡走 board.h 能力标志（`BOARD_HAS_*`）+ board.c 的 `#if` 分段。本板硬事实：单 I2S 总线（port 1，ES8311+ES7210 共享时钟，收发必须同采样率）；I2C port 1（SDA=15, SCL=14）；ES8311 地址 0x18、ES7210 0x40（esp_codec_dev 里传 **8-bit 左移值**）；触摸 FT3168（0x38）；显示 CO5300 410×502 QSPI；BOOT 键 GPIO0。
- **doubao_voice 组件**：单条 WSS（`wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue`）全双工。协议细节见设计文档 §4.3/§4.4 与 PRD 豆包 PDF/MD。关键点：`X-Api-Key` 头鉴权；上行 16k PCM / 下行 **固定 24k** PCM（不可改）；音频 Base64 进 JSON 文本帧；`session.id` 用于重连续接；打断 = `response.cancel`；退出意图 = `status_code 20000002`；协议无心跳需自建保活。

## ESP-IDF 开发铁律（本项目的坑，逐条遵守）

### 内存（最常翻车的领域）

1. **LVGL draw buffer 必须内部 RAM**（`MALLOC_CAP_INTERNAL`）；放 PSRAM = 白屏/花屏。沿用 Ver2.0 已验证配置：40 行单缓冲 ≈33-40KB 内部 RAM。
2. **DMA 缓冲必须内部 RAM**（I2S/SPI/摄像头等）：`MALLOC_CAP_DMA` 不含 PSRAM；I2S 驱动对象若落入 PSRAM 会直接起不来（IDFGH-14800）。大体积冷数据（字体、音频环形缓冲）才放 PSRAM，且**静态分配**，禁止高频 malloc/free 碎片化。
3. **任务 TCB 必须内部 RAM**（FreeRTOS 硬性要求），栈可放 PSRAM 但要 `xTaskCreateStatic` + `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`；**栈深度单位是字节不是字**（IDF 与 Vanilla FreeRTOS 不同，移植代码别乘 4）。音频解码/处理任务栈 ≥16KB（8KB 必炸，minimp3 教训）。
4. `malloc()` 小分配默认落内部 RAM（ALWAYSINTERNAL=256）——大缓冲必须显式 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`，否则内部堆耗尽时"莫名"malloc 失败。
5. flash 擦写期间 cache 禁用，**PSRAM 完全不可访问**——音频/网络热路径别依赖 PSRAM 数据。
6. 排查工具：`heap_caps_get_free_size(MALLOC_CAP_8BIT)` 看内部堆（esp_get_free_heap_size 含 PSRAM 会误导）；`heap_caps_check_integrity_all()`；`CONFIG_HEAP_POISONING_COMPREHENSIVE` 抓越界写。

### 任务与中断

7. app_main 优先级只有 1（很低）——重活任务用 `xTaskCreatePinnedToCore` 钉核 + 提优先级。本项目优先级纪律：**播放 > 采集 > WS 网络 > UI**；周期任务用 `xTaskDelayUntil`（vTaskDelay 会累积漂移）。
8. ISR / I2S 事件回调 / WS 事件回调内**禁止**：ESP_LOGx、malloc、printf、锁、阻塞、复杂逻辑、浮点。只做"拷贝进 ring buffer + 发信号"，重活抛给任务。ISR 日志用 `ESP_DRAM_LOGx`。
9. 音频实时性要求开 `CONFIG_I2S_ISR_IRAM_SAFE`（否则 flash 擦写期间音频卡顿/丢帧）。
10. 任务看门狗默认 5s：大段 flash 擦除、长临界区会触发 panic。状态机各状态必须有超时回收（本项目：思考 30s / 播报 60s / commit 后 15s 无输出 / 播报中 5s 无音频——按状态细分，聆听态不设下行超时）。

### I2S 与音频（全双工专项）

11. **同一 I2S 外设的 TX/RX 共享时钟，收发采样率必须一致**；采样率切换 = `i2s_channel_disable()` → reconfig → `enable()`，运行中变速一律软件重采样（disable/enable 会 flush DMA 造成卡顿）。
12. 本板 16kHz 全双工：TX/RX 用同一 std_cfg；`bit_shift=true, left_align=true`；TX `SLOT_BOTH`、RX 必须 `SLOT_RIGHT`（ES7210 mic 数据在右槽）；RX `channel=2` + `channel_mask=MAKE_CHANNEL_MASK(1)` 提取单声道，否则读到垃圾。
13. 录音/播放各放独立任务 + ring buffer 解耦（网络抖动由缓冲垫吸收）；DMA 总量 80-150ms；`tx_desc_auto_clear=true` 防旧数据；`i2s_channel_write/read` 是阻塞调用，超时返回 `ESP_ERR_TIMEOUT` 要处理。
14. **爆音 pop = DC 阶跃**：I2S 启动/停止前先写 50-100ms 静音帧渐变，结束补静音再 disable。
15. 回声分层（设计文档 §3.2）：v1 播放感知能量门控；后备方案为 **esp-sr 的 AEC**（`AEC_MODE_FD_LOW_COST`，仅 16kHz，参考信号必须取"真正送入 I2S 的、含音量缩放后的 PCM"并补偿 ~80ms 声学延迟）——不是 esp-adf。
16. esp_codec_dev：codec 初始化顺序 = 先 I2C 配 codec → 再 I2S 通道 enable（反了 codec 收不到有效时钟）；两个 codec 各自句柄（playback/record），I2S 通道只初始化一次；同一句柄不并发 read+write。

### LVGL 9 + esp_lvgl_port

17. `lv_display_flush_ready()` 必须在 DMA 完成回调（`on_color_trans_done`）里调用，不能在 flush_cb 里直接调（否则撕裂/花屏）。
18. 旋转优先**硬件旋转**（esp_lcd swap_xy/mirror，性能好）；LVGL9 `lv_disp_set_rotation()` 会自动旋转触摸坐标，无需手动映射；软件旋转（sw_rotate）慢且耗 RAM。
19. 气泡列表动态创建/删除会碎片化：**上限 50 条气泡，超出删最旧**；不做虚拟化。
20. 中文字体用 `lv_font_conv --format lvgl` 生成子集编 .c 进 flash 只读区（GB2312 一级 3755 字 + 12/16/24pt 三档）；**不要用 `--format bin` + `lv_font_load()`**（全量字形进 RAM + lv_font_conv 1.5.3 映射错乱，lvgl#7232）；不可渲染字符降级"□"不崩溃。

### 网络 / TLS / WebSocket

21. **WS ping 节奏**（ws-auth-ping-fix 教训）：认证期 `ping_interval_sec=1`，`WEBSOCKET_EVENT_CONNECTED` 后放宽到 30s，**每次重连都要重置回 1s**——否则服务端 10s 握手超时必断。
22. WS 事件回调内禁止 `esp_websocket_client_stop/close/destroy`（会返回失败/use-after-free）；控制动作发到别的任务执行，销毁前先注销事件处理器。
23. WS `buffer_size` 默认 1024B，超长消息自动分片：必须按 `payload_offset/payload_len` 重组帧（豆包下行 Base64 音频帧可达数十 KB）；上行 JSON 帧同理注意分段发送。
24. 证书：豆包用 `esp_crt_bundle_attach`（内置根证书库）；验证失败时改固定单 CA（cert_pem 直传）最可靠；SNTP 必须先校时否则证书有效期校验失败。
25. mbedTLS：开 `CONFIG_MBEDTLS_DYNAMIC_BUFFER` + 关 `MBEDTLS_SSL_KEEP_PEER_CERTIFICATE`（省 ~4KB）；SSL in/out content len 上限 16KB；**S3 保持硬件 AES/SHA 默认关闭**，别手动开。
26. WiFi：socket/WS 工作必须等 `IP_EVENT_STA_GOT_IP`（CONNECTED 不代表有 IP）；初始化顺序 esp_netif_init → esp_event_loop_create_default → create_default_wifi_sta → esp_wifi_init；断连时 lwIP 杀掉所有 TCP——在 `WIFI_EVENT_STA_DISCONNECTED` 里销毁 WS 句柄，GOT_IP 后重建；`esp_wifi_connect()` 只尝试一次，重连退避自己实现（前几次立即、之后递增、封顶）。
27. 退避等待中用户唤醒/单击 → **取消退避立即重连一次**（设计文档 §4.2）。

### NVS / 构建 / 日志

28. NVS：键与命名空间 ≤15 字符；只存低频配置（API Key/音量/音色），**禁止高频写或存音频日志**；初始化报 `NO_FREE_PAGES/NEW_VERSION_FOUND` 必须 `nvs_flash_erase()` 重试。
29. **CMake 两遍扫描**：`idf_component_register` 里禁止 `if(CONFIG_*)` 条件注册（枚举时 sdkconfig 为空）；条件编译只在 C 代码 `#if`。组件依赖：公共头用的进 `REQUIRES`，源文件用的进 `PRIV_REQUIRES`。
30. `managed_components/` 与 `dependencies.lock` 禁止手改；本地覆盖组件放 `components/`；搬动组件后 `idf.py reconfigure`。
31. 错误处理：可恢复错误用 `ESP_RETURN_ON_ERROR`，致命用 `ESP_ERROR_CHECK`；`esp_err_t` 不用 `assert()`；日志标签区分等级，动态开日志用 `esp_log_level_set()`。

## 调试与排障

- panic 后：`idf.py monitor` 自动解码地址为 文件:行号；手动 `xtensa-esp32s3-elf-addr2line -pfiaC -e build/*.elf <addr>`。
- 堆泄漏：`CONFIG_HEAP_TRACING_STANDALONE` + `heap_trace_init_standalone()`（记录缓冲必须内部 RAM 静态数组）→ `heap_trace_dump()`；WiFi/TCPIP 瞬时缓冲会被误报。
- 复位原因：启动即记录 `esp_reset_reason()`。
- 项目自带工具：串口 CLI（talk / doubao status / say <文本> / reboot）；mem_monitor 任务（内存高水位）；error_log 组件（32 条环形缓冲）；webserver 网页 `/api/errors`。
- 硬件排障顺序（PRD integration-test 验收序列）：Board → I2C（用逻辑分析仪确认地址，7-bit 左移混淆是第一嫌疑）→ codec（先查 MCLK 再查增益）→ I2S → WiFi（GOT_IP）→ SNTP → WSS 连接 → Ready。
- PC 音频模拟测试（PRD audio-emulation.md）：扬声器模拟语音输入，静音阈值 80 RMS、开始蜂鸣后延迟 100ms 采音。

## 参考文档索引

- 设计文档：`docs/superpowers/specs/2026-08-16-doubao-voice-robot-design.md`
- 豆包 API：`PRD/Project_Resources/豆包语音_端到端实时语音-全双工版本*.pdf/.md`
- 旧项目经验（21 篇，构建/内存/音频/UI/睡眠/测试）：`PRD/Project_Resources/*.md`，其中必读：adding-hardware-targets、audio-codec-dev、esp32-memory-architecture、tts-playback、ws-auth-ping-fix、integration-test
- 骨架参考代码：`/home/conor/esp/esp32/AIWatch_Ver2.0/`（board_amoled_206.h、app_state_machine、voice_chat.c 的 VAD、tts 环形缓冲模式）
