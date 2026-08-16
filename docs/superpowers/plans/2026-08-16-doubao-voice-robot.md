# AIWatch Doubao 语音机器人 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 ESP32-S3-Touch-AMOLED-2.06 上实现基于豆包端到端实时语音 API 的桌面 AI 语音对话机器人（WSS 全双工 + 唤醒词 + 气泡 UI）。

**Architecture:** 复制 AIWatch_Ver2.0 骨架（同板卡已验证），删除 openclaw/stt/tts/sscma_client，新建 `doubao_voice` 组件（单 WSS 全双工协议客户端）替换三件套，UI 重做为对话气泡列表，main 状态机改造为全双工（聆听+播报可同时）。

**Tech Stack:** ESP-IDF v5.5.5、LVGL 9.5.0（esp_lvgl_port 2.6.0）、esp_websocket_client 1.1.0、esp_codec_dev 1.5.0、esp-sr（WakeNet "你好小智"）、cJSON、mbedTLS。

**Spec:** `docs/superpowers/specs/2026-08-16-doubao-voice-robot-design.md`（实施前必读；协议细节见其 §4.3/§4.4 与 `PRD/Project_Resources/豆包语音_端到端实时语音-全双工版本*.pdf/.md`）

## Global Constraints

- 骨架来源：`/home/conor/esp/esp32/AIWatch_Ver2.0/`（复制后改造，不改原目录）
- 项目重命名：`HEYCLAWY` → `AIDB`（Kconfig `CONFIG_AIDB_BOARD_AMOLED_206`、NVS 命名空间 `"aidb"`、menuconfig 菜单 "AIWatch Doubao Application Configuration"）
- 板卡硬事实：单 I2S 总线 port 1（ES8311+ES7210 共享时钟，收发必须同采样率 16kHz）；I2C port 1 SDA=15/SCL=14；ES8311 I2C 地址 0x18、ES7210 0x40（esp_codec_dev 传 8-bit 左移值）；FT3168 触摸（0x38）；CO5300 410×502 QSPI 屏；BOOT 键 GPIO0
- 豆包 API：`wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue`；鉴权 `X-Api-Key` 头；上行 16k PCM、下行**固定 24k** PCM（Base64 进 JSON 文本帧）；model=`1.2.6.1`；打断=`response.cancel`；退出意图 status_code=`20000002`
- 内存铁律（CLAUDE.md 全文必读）：LVGL/DMA 缓冲内部 RAM；任务 TCB 内部 RAM、栈可 PSRAM（≥16KB）；大缓冲 PSRAM 静态分配；mbedtls 动态缓冲 + 关硬件 AES
- 任务优先级纪律：播放 > 采集 > WS 网络 > UI；WS/ISR 回调内禁止阻塞/日志/锁
- 错误处理：`esp_err_t` + `ESP_RETURN_ON_ERROR`；禁止硬编码 API Key
- 提交规范：每个任务完成即 commit，信息带 `Co-Authored-By: Claude <noreply@anthropic.com>`
- 测试回路：嵌入式 C——纯逻辑件（协议解析/重采样/环形缓冲）用串口测试命令在目标板验证；集成件用 `idf.py build` + `flash` + `monitor` 日志断言验收；无硬件时至少保证 `idf.py build` 通过
- 串口：板载原生 USB-JTAG，端口 `/dev/ttyACM0`（以 `idf.py monitor` 自动检测为准）；CH342 芯片必须 rts=False（若用外接串口）

---

## M1：骨架移植

### Task 1: 复制骨架 + 裁剪 + 重命名

**Files:**
- Create: 整个工程树（从 `/home/conor/esp/esp32/AIWatch_Ver2.0/` 复制到本项目根，排除 `build/`、`managed_components/`、`sdkconfig.old`、`esp_log.txt`、`*.corrupt.bak`、`.git`）
- Delete: `components/openclaw/`、`components/stt/`、`components/tts/`、`components/sscma_client/`、`components/ed25519_lib/`、`main/voice_chat.c`（本任务只删，重写见 Task 7）、`components/board/esp_lcd_panel_jd9853.*`（其他板遗留）及所有 `#include` 引用
- Modify: 全局字符串替换（`main/`、`components/` 全部 .c/.h/CMakeLists.txt/Kconfig*）：
  - `HEYCLAWY` → `AIDB`（Kconfig 宏、NVS 命名空间字符串 `"heyclawy"` → `"aidb"`、`CONFIG_HEYCLAWY_*` → `CONFIG_AIDB_*`）
  - `HeyClawy` → `AIWatch Doubao`（menuconfig 菜单标题）
  - `AIClaw`/`AIWearable` → `AIWatchDB`（仅 LOG 文案与注释）
- Modify: 根 `CMakeLists.txt` 的 project 名 → `aiwatch_doubao`；`main/idf_component.yml`、`main/CMakeLists.txt` 的 REQUIRES 删除 `openclaw stt tts`；`components/*/CMakeLists.txt` 里对已删组件的依赖同步删除
- Modify: 删除 `main/app_main.c`、`main/app_tasks.c`、`main/app_state.c`、`main/app_state.h` 中对 openclaw/stt/tts/voice_chat 的调用（openclaw_init、stt_init、tts_init、voice_chat_start、OPENCLAW_* 事件位、tts_play_task 等——**本任务全部注释掉并加 `// TODO(Task 7/8/10): 由 doubao 链路替换**，不要改状态机逻辑本身）
- **保留不动**：`mp3_player` 及其音频输出仲裁逻辑（spec §6.3）、`wake_word`、`wifi_manager`、`webserver`、`settings`、`notes_manager`、`error_log`、`app_state.c` 内的 `parse_device_commands()`（[DEVICE:] 语音控制）

**Interfaces:**
- Consumes: 无（纯机械操作）
- Produces: 可编译的空对话链路骨架；`CONFIG_AIDB_BOARD_AMOLED_206` Kconfig 选项；NVS 命名空间 `"aidb"`

- [ ] **Step 1: 复制骨架**

```bash
cd /home/conor/esp/esp32/AIWatch_Doubao
cp -r /home/conor/esp/esp32/AIWatch_Ver2.0/{main,components,partitions.csv,sdkconfig.defaults,CMakeLists.txt,dependencies.lock,README.md} ./
```

- [ ] **Step 2: 删除废弃组件与文件**（上述 Delete 列表，`git rm` 或 `rm`）
- [ ] **Step 3: 全局重命名**（`grep -rl HEYCLAWY --include='*.[ch]' --include='CMakeLists.txt' --include='Kconfig*' . | xargs sed -i 's/HEYCLAWY/AIDB/g'`，其余两对同理；NVS 字符串 `"heyclawy"` → `"aidb"` 单独处理）
- [ ] **Step 4: 修正 CMake 依赖**（REQUIRES 删 openclaw/stt/tts；删除对 `components/openclaw/include` 等的 include 路径）
- [ ] **Step 5: 注释 main/ 中已删组件的调用**（按上述 Modify 规则，保留 TODO 标记）
- [ ] **Step 6: 验证构建通过**

```bash
idf.py set-target esp32s3 && idf.py build
```
Expected: 构建成功（警告可容忍，错误必须清零）

- [ ] **Step 7: Commit**

```bash
git add -A && git commit -m "refactor: port Ver2.0 skeleton, strip openclaw/stt/tts, rename to AIDB

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 2: 烧录 + 启动序列验收 + 旋转/触摸验证

**Files:**
- Modify: `sdkconfig.defaults`（`CONFIG_AIDB_BOARD_AMOLED_206=y`）
- Modify: `components/board/board.c` 的显示初始化（加 `esp_lcd_panel_mirror(true, true)` 或按 PRD/board 头建议的 swap_xy/mirror 配置实现横屏——若 BSP 不支持旋转，改 `lv_disp_set_rotation(LV_DISPLAY_ROTATION_90)` 并验证触摸坐标）

**Interfaces:**
- Consumes: Task 1 的可构建骨架
- Produces: 已烧录可运行固件；横屏显示 + 触摸坐标对齐验证结论（记录在 PRD 或 commit message）

- [ ] **Step 1: menuconfig 选板**

```bash
idf.py menuconfig   # "AIWatch Doubao Application Configuration" → Board target = ESP32-S3-Touch-AMOLED-2.06
```
- [ ] **Step 2: 烧录**

```bash
idf.py -p /dev/ttyACM0 -b 921600 flash monitor
```
- [ ] **Step 3: 启动序列日志验收**（对照 PRD integration-test 清单，逐条确认）
Expected 日志序列: `board_init OK → I2C OK → codec OK → display OK → LVGL OK → wifi_manager started → webserver started → [openclaw/stt/tts 相关日志已消失]`
- [ ] **Step 4: 旋转验证**：屏幕横屏显示、四角触摸坐标与显示位置一致（用现有 UI 元素或临时加 `ui_set_response("touch test")` 打点验证）；不一致则修旋转配置直至一致
- [ ] **Step 5: Commit**

```bash
git commit -am "feat: AMOLED landscape rotation + touch mapping verified

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 3: 全双工硬件回路测试

**Files:**
- Create: `components/board/board_loopback_test.c`（临时测试模块，注册串口命令 `audio loop`；**M3 完成后删除**）
- Modify: `main/serial_cmd.c`（注册 `audio loop` 命令）

**Interfaces:**
- Consumes: `board_audio_record(int16_t*, size_t)`、`board_audio_play(const int16_t*, size_t)`（board.h 既有签名，沿用）
- Produces: 结论数据：共享时钟下同时播放+录音的稳定性（丢帧率/爆音有无），写入 commit message

- [ ] **Step 1: 写回路测试模块**

```c
// board_loopback_test.c 核心逻辑
static void loopback_task(void *arg) {
    int16_t *play = heap_caps_malloc(3200, MALLOC_CAP_INTERNAL);  // 100ms@16k
    int16_t *rec  = heap_caps_malloc(3200, MALLOC_CAP_INTERNAL);
    for (int i = 0; i < 1600; i++) play[i] = (int16_t)(sinf(i * 2 * M_PI * 440 / 16000) * 8000);
    while (true) {
        int r = board_audio_record(rec, 1600);        // 100ms 块
        int w = board_audio_play(play, 1600);          // 同时播放 440Hz
        if (r < 1600 || w < 1600) ESP_LOGW(TAG, "underrun r=%d w=%d", r, w);
    }
}
```
（实际用 PRD audio-codec-dev.md 的 mutex 包裹 codec 读写模式；任务栈 8KB 内部 RAM，钉 core 1）

- [ ] **Step 2: 注册串口命令**：`audio loop` 启停该任务
- [ ] **Step 3: 烧录运行 10 分钟**
Expected: 扬声器稳定输出 440Hz 正弦、无爆音；日志无 underrun 警告；mic 采到的电平正常（另一条命令 `audio rec` 打印 RMS）
- [ ] **Step 4: Commit**

```bash
git commit -am "test: full-duplex I2S loopback test (ES8311+ES7210 shared clock OK)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## M2：doubao_voice 协议链路

### Task 4: doubao_voice 组件骨架

**Files:**
- Create: `components/doubao_voice/idf_component.yml`：

```yaml
dependencies:
  espressif/esp_websocket_client: "^1.1.0"
  idf: ">=5.5.0"
```

- Create: `components/doubao_voice/CMakeLists.txt`：

```cmake
idf_component_register(SRCS "doubao_voice.c" "protocol.c" "ws_client.c" "resampler.c"
    INCLUDE_DIRS "include" REQUIRES esp_websocket_client json esp_timer esp_event)
```

- Create: `components/doubao_voice/include/doubao_voice.h`（完整接口，见下）
- Create: `components/doubao_voice/doubao_voice.c`（状态 + 队列 + 回调分发 + 各函数空/桩实现）
- Modify: 根 `CMakeLists.txt` 的 EXTRA_COMPONENT_DIRS 无变化（components/ 自动识别）；`main/CMakeLists.txt` REQUIRES 加 `doubao_voice`

**Interfaces:**
- Produces（全项目锁定的接口，后续任务全部依赖）：

```c
typedef enum {
    DOUBAO_EVT_CONNECTED, DOUBAO_EVT_DISCONNECTED,
    DOUBAO_EVT_SESSION_CREATED,      // data: 无（用 doubao_get_session_id()）
    DOUBAO_EVT_TRANSCRIPT_DELTA,     // data: const char* 用户文本片段
    DOUBAO_EVT_TRANSCRIPT_DONE,      // data: const char* 完整识别文本
    DOUBAO_EVT_OUTPUT_TEXT_DELTA,    // data: const char* 回复文本片段
    DOUBAO_EVT_OUTPUT_TEXT_DONE,     // data: const char* 完整回复
    DOUBAO_EVT_AUDIO_STARTED,        // data: const char* tts_type
    DOUBAO_EVT_AUDIO_DELTA,          // data: doubao_audio_chunk_t*（PCM 24k/16bit，PSRAM 缓冲，回调返回后即失效）
    DOUBAO_EVT_AUDIO_DONE,           // data: int* status_code（20000002=退出意图）
    DOUBAO_EVT_RESPONSE_DONE,        // data: 无（本轮结束）
    DOUBAO_EVT_INTERRUPTED,          // data: 无（服务端确认打断）
    DOUBAO_EVT_ERROR,                // data: const char* 错误描述
} doubao_event_type_t;

typedef struct { const int16_t *pcm24; size_t samples; } doubao_audio_chunk_t;
typedef struct {
    const char *api_key;      // X-Api-Key
    const char *voice;        // 如 "zh_female_vv_jupiter_bigtts"
    const char *instructions; // 系统提示词
    int8_t speed;             // [-50,100]
    int8_t loudness;          // [-50,100]
} doubao_cfg_t;
typedef void (*doubao_event_cb_t)(doubao_event_type_t type, const void *data, size_t len);

esp_err_t doubao_init(const doubao_cfg_t *cfg, doubao_event_cb_t cb);
esp_err_t doubao_connect(void);
esp_err_t doubao_disconnect(void);
bool doubao_is_connected(void);
esp_err_t doubao_send_audio(const int16_t *pcm16, size_t samples); // 上行帧，异步入队
esp_err_t doubao_commit_audio(void);
esp_err_t doubao_interrupt(void);
esp_err_t doubao_clear_session(void);
esp_err_t doubao_push_text(const char *text); // speech_text_buffer 直推（say 命令）
const char *doubao_get_session_id(void);
```

- [ ] **Step 1: 写头文件与 idf_component.yml/CMakeLists.txt**（内容如上，逐字）
- [ ] **Step 2: 写 doubao_voice.c 桩实现**（每个函数返回 ESP_ERR_NOT_SUPPORTED 并 ESP_LOGW；init 保存 cfg；事件回调空转）
- [ ] **Step 3: 构建验证**

```bash
idf.py build
```
Expected: 编译通过，组件注册成功

- [ ] **Step 4: Commit**

```bash
git add components/doubao_voice main/CMakeLists.txt && git commit -m "feat: doubao_voice component skeleton with locked API

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 5: protocol.c 编解码（含分片重组）

**Files:**
- Create: `components/doubao_voice/protocol.c` + `components/doubao_voice/include/doubao_protocol.h`
- Create: `main/proto_test.c`（临时串口测试命令 `proto test`，M2 结束可保留至 M5 再删）

**Interfaces:**
- Consumes: Task 4 的事件枚举与回调签名
- Produces:

```c
// doubao_protocol.h
esp_err_t proto_build_session_create(char *buf, size_t cap, const doubao_cfg_t *cfg, const char *session_id /*可 NULL*/);
esp_err_t proto_build_audio_append(char *buf, size_t cap, const int16_t *pcm16, size_t samples, uint32_t event_id);
esp_err_t proto_build_commit(char *buf, size_t cap);
esp_err_t proto_build_cancel(char *buf, size_t cap);
esp_err_t proto_build_close(char *buf, size_t cap);
esp_err_t proto_build_text_push(char *buf, size_t cap, const char *text);
typedef struct { char *data; size_t len; } ws_frag_t;   // 分片重组
esp_err_t proto_feed(ws_frag_t frag, doubao_event_cb_t cb); // 流式解析，事件即回调
void proto_reset(void);
```

- [ ] **Step 1: 写上行构造函数**（固定静态缓冲 + mbedtls base64：`mbedtls_base64_encode`）

```c
// 上行 JSON 模板（一次 snprintf 拼装，无动态分配）
// {"type":"input_audio_buffer.append","audio":"%s"}
// {"type":"session.create","session":{"model":"1.2.6.1","instructions":"%s",
//   "audio":{"input":{"format":{"type":"pcm","rate":16000}},
//            "output":{"format":{"type":"pcm","rate":24000},"voice":"%s","speed":%d,"loudness":%d}},
//   "id":"%s"}}   ← id 仅重连续接时带
```
- [ ] **Step 2: 写下行流式解析**：按 WS 分片边界累积到静态 PSRAM 缓冲（64KB），每次找到完整 JSON（`{` 与 `}` 配对、引号内转义 `\"` 正确处理）即 `cJSON_Parse` → 按 `type` 字段分发回调；解析失败仅 ESP_LOGW 不崩
- [ ] **Step 3: 事件→回调映射表**（`type` 字符串 → `DOUBAO_EVT_*`；`response.output_audio.delta` 的 `delta` 字段 mbedtls_base64_decode 到静态 PSRAM PCM 缓冲）
- [ ] **Step 4: proto_test.c 测试命令**：内置 5 个豆包文档中的真实下行事件 JSON 样本（transcript.delta / output_text.delta / audio.delta / audio.done(20000002) / error），逐个 `proto_feed` 并断言回调类型与数据正确，结果打印 `proto test: PASS/FAIL`
- [ ] **Step 5: 烧录运行验证**

```bash
idf.py build flash monitor   # 串口输入: proto test
```
Expected: `proto test: PASS`

- [ ] **Step 6: Commit**

```bash
git commit -am "feat: doubao protocol encode/decode with fragmentation reassembly + on-target tests

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 6: ws_client.c（WSS 连接管理）

**Files:**
- Create: `components/doubao_voice/ws_client.c` + `components/doubao_voice/include/doubao_ws_client.h`
- Modify: `components/doubao_voice/doubao_voice.c`（接入 ws_client 生命周期）

**Interfaces:**
- Consumes: Task 4 配置/回调、Task 5 `proto_build_*` 与 `proto_feed`
- Produces:

```c
// doubao_ws_client.h
esp_err_t dbws_start(const doubao_cfg_t *cfg, doubao_event_cb_t cb);  // 启动后台任务：连接→session.create→收发循环
esp_err_t dbws_stop(void);          // 幂等
void dbws_request_reconnect(void);  // 退避等待中调用=立即重连
bool dbws_is_connected(void);
esp_err_t dbws_send_frame(const char *frame, size_t len);  // 发送一帧 JSON（线程安全，内部队列）
const char *dbws_get_session_id(void);
```

- [ ] **Step 1: 写 WSS 客户端**：`esp_websocket_client_config_t` = `uri`（豆包端点）、`headers` 数组（`{"X-Api-Key", cfg->api_key}`）、`cert_pem=NULL` + `crt_bundle_attach`（esp_crt_bundle_attach）、`buffer_size=4096`、`ping_interval_sec=1`（认证期）
- [ ] **Step 2: 事件回调**（只做轻量工作）：`WEBSOCKET_EVENT_CONNECTED` → 置连接标志、发 `proto_build_session_create(带上次 session.id 若存在)`、ping 间隔放宽 30s（`esp_websocket_client_set_config` 或重建配置）；`WEBSOCKET_EVENT_DATA` → 按 `payload_offset/payload_len` 组帧喂 `proto_feed`；`WEBSOCKET_EVENT_DISCONNECTED/ERROR` → 置断连标志、发 `DOUBAO_EVT_DISCONNECTED`、**每次重连前 ping 重置回 1s**
- [ ] **Step 3: 后台任务**：指数退避重连（2→60s 封顶；`dbws_request_reconnect()` 置立即重连标志取消当前退避）；退避循环 `vTaskDelay(pdMS_TO_TICKS(backoff*1000))`；`session.created` 事件保存 `session.id`（协议字段 `session.id`，Task 5 回调里提取）
- [ ] **Step 4: 串口命令 `doubao connect`/`doubao disconnect` 验证**（serial_cmd.c 注册）：连上后日志断言 `session.created, session.id=<id>`（需真实 API Key，用临时 secrets.h 注入，测后删除）
- [ ] **Step 5: 断线重连验证**：关闭路由器 → 日志出现退避序列 → 恢复网络 → 自动重连且日志显示 `reconnect with session.id=<id>`
- [ ] **Step 6: Commit**

```bash
git commit -am "feat: doubao WSS client with reconnect backoff + session resume

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 7: 文本链路 E2E（say 命令 → 气泡流式显示）

**Files:**
- Create: `main/doubao_chat.c` + `main/include/doubao_chat.h`（对话编排雏形，本任务只做文本链路）
- Modify: `main/app_main.c`（`doubao_init()` 带 settings 配置；状态机事件位挂 `DOUBAO_*` 回调；串口 `say <文本>` 命令路由）
- Modify: `components/ui/include/ui.h` + `ui_main.c`（最小气泡容器：`ui_add_user_bubble(const char*)`、`ui_add_bot_bubble(void)`、`ui_bot_bubble_append(const char*)` 三函数 + 状态胶囊文本更新 `ui_set_state()` 沿用）

**Interfaces:**
- Consumes: Task 4-6 全部接口；`settings` 组件（新增字段见 Task 17，本任务先用 `secrets.h` 占位值）
- Produces: `doubao_chat.h`：

```c
void doubao_chat_start(void);          // 事件组 DOUBAO_START_BIT 触发（say 命令/唤醒共用入口，本任务只接 say）
void doubao_chat_on_event(doubao_event_type_t type, const void *data, size_t len); // doubao_init 注册的回调
```

- [ ] **Step 1: 写 doubao_chat.c 回调分发**：OUTPUT_TEXT_DELTA → `ui_bot_bubble_append()`；TRANSCRIPT_* → `ui_add_user_bubble()`（TRANSCRIPT_DONE 时整体替换）；OUTPUT_TEXT_DONE → 状态置 IDLE、**复用 Ver2.0 `app_state.c` 的 `parse_device_commands()`（[DEVICE:] 语音控制）与 notes_manager 对话记录存储**（从原 on_chat_response 移植过来）；ERROR → `ui_set_state(ERROR)` + error_log
- [ ] **Step 2: say 命令链路**：`say 你好` → `doubao_push_text("你好")`（内部 proto_build_text_push → speech_text_buffer.commit 事件）→ 服务端返回 output_text.delta → 气泡流式追加
- [ ] **Step 3: UI 最小实现**：气泡 = lv_obj 容器 + lv_label（长文本 `lv_label_set_long_mode(LV_LABEL_LONG_WRAP)`），放入可滚动 `lv_obj` 滚动容器；`ui_bot_bubble_append` 用 `lv_label_set_text_fmt` 累加重设（本任务先用整段重设，流式优化在 Task 13）
- [ ] **Step 4: 烧录验证**

```bash
idf.py build flash monitor   # 串口: say 你好，今天天气怎么样？
```
Expected: 屏幕出现用户气泡（推送文本）+ 机器人气泡流式增长；无 OOM（monitor 里 mem_monitor 数值稳定）

- [ ] **Step 5: Commit**

```bash
git commit -am "feat: text E2E via say command with streaming bot bubble

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## M3：完整语音链路

### Task 8: 上行采集任务与发送队列

**Files:**
- Create: `components/doubao_voice/audio_in.c` + `include/doubao_audio_in.h`
- Modify: `components/doubao_voice/doubao_voice.c`（`doubao_send_audio` 真实实现：入队）

**Interfaces:**
- Consumes: `board_audio_record()`；Task 5 `proto_build_audio_append`
- Produces:

```c
// doubao_audio_in.h
esp_err_t dbaudio_in_start(void);   // 创建采集任务（钉 core 1，prio 9，栈 8KB 内部 RAM）
esp_err_t dbaudio_in_stop(void);
esp_err_t dbaudio_in_reset_queue(void);  // 对话开始时清空陈旧队列
```

- [ ] **Step 1: 上行队列**：静态 PSRAM 环形缓冲，**12 帧 × 640 样本（40ms）**；溢出丢最旧帧 + `s_overflow_cnt++` 计数（每分钟 ESP_LOGW 一次）
- [ ] **Step 2: 采集任务循环**：`board_audio_record(buf, 640)` → 拷贝入队 → 若 WS 已连接，发送任务从队列取帧 → `proto_build_audio_append` → `dbws_send_frame`；发送在 **WS 后台任务内**完成（Task 6 的任务循环里加"取音频帧发送"步骤），采集任务绝不阻塞在网络
- [ ] **Step 3: 验证**（Task 9 完成后一起验收，本任务先 build 通过 + `idf.py build`）
- [ ] **Step 4: Commit**

```bash
git commit -am "feat: uplink capture task + 12-frame send queue with overflow drop-oldest

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 9: VAD 与判停

**Files:**
- Create: `main/vad.c` + `main/include/vad.h`（从 Ver2.0 `voice_chat.c:139-243` 移植自适应噪声底算法）
- Modify: `main/doubao_chat.c`（对话编排：start → 采集 → VAD → commit）

**Interfaces:**
- Consumes: Task 8 `dbaudio_in_*`；`doubao_commit_audio()`
- Produces:

```c
// vad.h
void vad_init(int16_t silence_ms, int16_t max_record_ms); // 默认 1500 / 15000
typedef enum { VAD_SPEECH, VAD_SILENCE, VAD_MAX_TIMEOUT } vad_state_t;
vad_state_t vad_process(const int16_t *pcm, size_t samples); // 每 40ms 帧调一次
float vad_rms(void);   // 当前 RMS（打断检测复用，Task 11）
```

- [ ] **Step 1: 移植 VAD**：16 帧噪声底校准 + DC 偏移消除 + 自适应阈值；静音持续 1.5s → `VAD_SILENCE`；总时长 15s → `VAD_MAX_TIMEOUT`
- [ ] **Step 2: doubao_chat.c 对话流程**：`doubao_chat_start()` → `dbaudio_in_reset_queue()` + `dbaudio_in_start()` + 状态 LISTENING → VAD 判停 → `doubao_commit_audio()` → 状态 THINKING；无语音（<0.3s 且 RMS 低）→ 回 IDLE
- [ ] **Step 3: 烧录验证**：`say` 改为 `talk` 命令触发（`talk` = 完整对话流程）；说话后 1.5s 静音自动判停，气泡出现识别文本与回复
Expected: 说话 → 判停 → 服务端识别 → 回复文本流式出现；对安静环境不说话 15s → 自动判停回 IDLE
- [ ] **Step 4: Commit**

```bash
git commit -am "feat: adaptive VAD port + commit-based turn detection

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 10: 重采样器 + 下行播放链路

**Files:**
- Create: `components/doubao_voice/resampler.c` + `include/doubao_resampler.h`
- Create: `components/doubao_voice/audio_out.c` + `include/doubao_audio_out.h`
- Modify: `components/doubao_voice/doubao_voice.c`（AUDIO_DELTA 事件 → audio_out 入队）
- Modify: `main/doubao_chat.c`（状态 SPEAKING 联动；AUDIO_DONE → 本轮收尾）

**Interfaces:**
- Consumes: `board_audio_play()`；Task 5 的 PCM 解码产物
- Produces:

```c
// doubao_resampler.h —— 3:2 多相（24k→16k，定点 int32 中间量）
void resampler_init(resampler_t *r);  // 运行时算 48 抽头窗函数 sinc 系数（每相 8 抽头，6 相）
size_t resampler_process(resampler_t *r, const int16_t *in24, size_t n_in, int16_t *out16, size_t cap_out);
// 状态含历史样本尾 15 个，跨块连续；调用前 memset(&r,0,sizeof r)

// doubao_audio_out.h
esp_err_t dbaudio_out_push(const int16_t *pcm24, size_t samples); // 重采样后入播放环
esp_err_t dbaudio_out_start(void);   // 播放任务：钉 core 1，prio 10（最高），栈 16KB PSRAM
esp_err_t dbaudio_out_stop(void);    // 停播：50ms 渐变静音 → 停 I2S（防爆音）
void dbaudio_out_interrupt(void);    // 打断：立即停播（渐隐 20ms）
float dbaudio_out_current_rms(void); // 当前播放输出 RMS（Task 11 打断检测的参考能量）
```

- [ ] **Step 1: 重采样器**：64KB 静态 PSRAM 播放环形缓冲（存 16k PCM）；欠载补 50ms 渐变静音、过载丢最旧块（记高水位计数）
- [ ] **Step 2: 播放任务**：环空时 `vTaskDelay(10ms)` 等数据（不忙等）；`board_audio_play(块, 640)`；停止时先播 50ms 零帧渐变再停 codec（PRD tts-playback.md 模式）
- [ ] **Step 3: 重采样单测**：串口命令 `resample test` 输入 1kHz 正弦 24k → 输出 16k RMS/频率估算校验（过零率算频率，误差 <1%）
- [ ] **Step 4: 烧录验证**：`talk` 完整对话 → 回复播报流畅、无爆音、无卡顿
- [ ] **Step 5: Commit**

```bash
git commit -am "feat: 3:2 polyphase resampler + downlink playback ring with anti-pop fades

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 11: 打断 + 状态机全双工改造

**Files:**
- Modify: `main/app_state_machine.c/h`（状态机：允许 LISTENING 与 SPEAKING 叠加——播报中 mic 持续上行；RES_AUDIO_IN/OUT 双工仲裁；新状态 INTERRUPTING 可选合并进 LISTENING）
- Modify: `main/doubao_chat.c`（本地能量打断检测 + response.cancel）
- Modify: `components/doubao_voice/doubao_voice.c`（`doubao_interrupt()` 实现：发 cancel 帧 + 调 `dbaudio_out_interrupt()`）

**Interfaces:**
- Consumes: Task 9 `vad_rms()`、Task 10 `dbaudio_out_interrupt()`
- Produces: 打断语义：`DOUBAO_EVT_INTERRUPTED`（服务端 `response.canceled` / `input_audio_transcription.started` 到达时发）

- [ ] **Step 1: 本地打断检测**（doubao_chat.c 内，播报态每 40ms 帧调用）：播放参考能量 = 当前播放环输出块 RMS（dbaudio_out 暴露 `dbaudio_out_current_rms()`）；条件 `vad_rms() > play_rms + 6dB 且持续 80ms` → 触发打断
- [ ] **Step 2: 打断动作**：`doubao_interrupt()` → `response.cancel` 帧 + 播放渐隐 20ms 停 + 状态回 LISTENING（上行不中断）；服务端 `transcription.started` 到达 → `DOUBAO_EVT_INTERRUPTED` → UI 确认打断成功
- [ ] **Step 3: 状态机改造**：THINKING/SPEAKING 期间禁止 `dbaudio_in_stop()`；SPEAKING 时新 VAD 判停照常走 commit（无需重开采集）；响应超时看门狗按 spec §7（commit 后 15s 无输出 / 播报中 5s 无 audio delta）
- [ ] **Step 4: 烧录验证**：长回复播报中说话 → <100ms 停播、进入聆听、服务端识别新语音；连续 5 次打断状态机稳定（日志无异常状态跳转）
- [ ] **Step 5: Commit**

```bash
git commit -am "feat: local-energy barge-in with response.cancel + full-duplex state machine

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 12: 唤醒集成 + 自动续听 + 触摸唤醒

**Files:**
- Modify: `main/doubao_chat.c`（唤醒词事件 → start；自动续听：RESPONSE_DONE 后保持聆听，VAD 空闲 8s → 待机；20000002 → 直接待机）
- Modify: `main/app_main.c`（`wake_word` 的 WAKE_WORD_BIT → `doubao_chat_start()` 替换原 voice_chat 调用；wake_word_pause/resume 挂在对话开始/结束）
- Modify: `components/ui/ui_main.c`（单击触摸 → 唤醒事件位；双击 → 打断事件位——手势检测见 Task 14，本任务先用临时长按/单击区分）

**Interfaces:**
- Consumes: `wake_word` 组件既有接口（`wake_word_pause()/resume()` 等，以 Ver2.0 wake_word.h 实际签名为准）
- Produces: 完整唤醒→对话→待机闭环

- [ ] **Step 1: 唤醒词接线**：WAKE_WORD_BIT → `doubao_chat_start()`；start 内先 `wake_word_pause()`；回待机 `wake_word_resume()`（含 ERROR 路径）
- [ ] **Step 2: 自动续听**：RESPONSE_DONE 且 settings.auto_continue → 保持 LISTENING；`vad_process` 连续 8s 静音 → 待机 + `wake_word_resume()`；AUDIO_DONE(status=20000002) → 待机
- [ ] **Step 3: 触摸唤醒**：单击（待机态）→ 同唤醒词路径；单击（播报中）→ `doubao_interrupt()`；双击 → `doubao_interrupt()` + 回待机（spec §6.3）
- [ ] **Step 4: 烧录验证**：喊"你好小智"→ 对话；播报结束静音 8s → 回待机（唤醒词恢复、屏保计时启动）；说"再见/拜拜"类退出意图 → 回待机
- [ ] **Step 5: 删除 Task 3 的临时回路测试模块**（board_loopback_test.c 及串口注册）
- [ ] **Step 6: Commit**

```bash
git commit -am "feat: wake-word + touch wake, auto-continue listening, exit-intent standby

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## M4：UI 与配置

### Task 13: 气泡列表 UI 完整版 + 状态胶囊

**Files:**
- Modify: `components/ui/src/ui_main.c` + `include/ui.h`（重做主屏：横屏 502×410 布局）
- Modify: `components/ui/src/ui_settings.c`（若存在则改造，否则新建，Task 14 用）

**Interfaces:**
- Consumes: Task 7 的三个气泡函数（升级实现）
- Produces: `ui.h` 增补：

```c
void ui_set_bubble_limit(uint8_t max_bubbles); // 默认 50，超出删最旧 lv_obj（lv_obj_delete）
void ui_set_status(ui_status_t st);  // IDLE/LISTENING/THINKING/SPEAKING/RECONNECTING/ERROR，顶栏胶囊换色换字
void ui_show_typing_indicator(bool show); // 思考省略号气泡
void ui_clear_bubbles(void);         // 清空全部气泡（Task 14 清空对话用）
```

- [ ] **Step 1: 布局**：顶栏（32px 高：状态胶囊 + WiFi 图标 + 音量图标）→ 气泡滚动区（`lv_obj_set_scroll_dir(LV_DIR_VER)`，`lv_obj_scroll_to_view_recursive` 自动滚底）→ 底部提示条（20px："你好小智 / 单击开始"）
- [ ] **Step 2: 气泡样式**：用户右对齐深色圆角（radius 12，pad 8）、机器人左对齐浅色；宽度 = 屏宽×75%，`LV_LABEL_LONG_WRAP`；append 用增量文本累加（长文本重设整段在 >2KB 时卡顿，改用 `lv_label_set_text` 全量但仅当增量超过 512B 才重设，中间增量用临时尾部 label）
- [ ] **Step 3: 上限管理**：气泡数 > 50 → 从滚动区子对象最旧删起；内存验证：连续 100 轮对话 mem_monitor 无下降趋势
- [ ] **Step 4: Commit**

```bash
git commit -am "feat: chat bubble list UI with status capsule and 50-bubble cap

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 14: 触摸手势 + 轻设置页

**Files:**
- Modify: `components/ui/src/ui_main.c`（手势检测：单击/双击/长按 1s 状态机——用 lv_indev 事件 + 300ms 双击窗口 + 1s 长按计时器）
- Create: `components/ui/src/ui_settings_page.c`（轻设置页：音量滑块 lv_slider、清空对话按钮 lv_button、WiFi 状态 label、设备信息 label、网页配置提示 label）
- Modify: `main/app_state.c`（清空对话动作：`doubao_interrupt()` → `doubao_clear_session()` → `ui_clear_bubbles()` → notes_manager 清记录 → 回待机）

**Interfaces:**
- Consumes: `settings` 组件（音量读写）、`wifi_manager`（SSID/IP 查询）、`doubao_*`、`ui_*`
- Produces: 事件位：`UI_TAP_BIT`、`UI_DOUBLE_TAP_BIT`、`UI_LONG_PRESS_BIT`（替换 Task 12 的临时手势）

- [ ] **Step 1: 手势状态机**（ui 触摸回调内）：按下记录 tick → 抬起 <300ms 且无第二次按下 = 单击置 `UI_TAP_BIT`；300ms 内第二次按下抬起 = 双击置 `UI_DOUBLE_TAP_BIT`；按下持续 >1s = 长按置 `UI_LONG_PRESS_BIT`（并消费该次手势）
- [ ] **Step 2: 轻设置页**：长按进入（`lv_obj_add_flag(scr, LV_OBJ_FLAG_HIDDEN)` 切换两个 screen）；音量滑块值即改即存（settings）；"清空对话"按钮 → 置 `UI_CLEAR_BIT`；设备信息：固件版本 `esp_app_get_description()->version`、IP（wifi_manager 接口）、session 状态（`doubao_get_session_id()` 非空 = 已连接）；底部"配置请访问 http://<IP>"
- [ ] **Step 3: 手势接入**（app_main.c）：UI_TAP_BIT 按当前状态分发（spec §6.3 表）；UI_DOUBLE_TAP_BIT → 打断+待机；UI_LONG_PRESS_BIT → 设置页；UI_CLEAR_BIT → 清空流程
- [ ] **Step 4: 烧录验证**：三手势全部生效；设置页各项可用；清空对话后气泡清空、服务端上下文重置（下一轮不记得上一轮）
- [ ] **Step 5: Commit**

```bash
git commit -am "feat: tap/double-tap/long-press gestures + light settings page

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 15: 中文字体

**Files:**
- Create: `components/ui/fonts/cjk_12.c`、`cjk_16.c`、`cjk_24.c`（生成产物）
- Create: `components/ui/fonts/regenerate_fonts.sh`（GB2312 一级 3755 字 + ASCII + 常用标点；三档）
- Modify: `components/ui/src/ui_main.c`（`lv_obj_set_style_text_font` 应用三档字体；不可渲染字符降级"□"——`lv_font_conv` 生成的字体天然缺字显示豆腐块，无需额外处理）

**Interfaces:**
- Consumes: 无（静态字体文件）
- Produces: `lv_font_cjk_12/16/24` 三个 `lv_font_t*` 符号

- [ ] **Step 1: 写生成脚本**（照搬 Ver2.0 `fonts/regenerate_font.sh` 模式）：

```bash
#!/bin/bash
# lv_font_conv --font NotoSansSC-Regular.otf --size 16 --bpp 4 \
#   --format lvgl --symbols "$(cat gb2312_l1.txt)" -o cjk_16.c
```
（gb2312_l1.txt：一级字库 3755 字 + ASCII 95 字符 + `，。！？；：""''（）【】、·—…%℃` 常用标点）

- [ ] **Step 2: 运行脚本生成三档字体**，确认 flash 占用（`idf.py size` 前后对比，三档合计应 <1MB）
- [ ] **Step 3: UI 应用**：状态栏 12pt、气泡 16pt、大字号提示/屏保时钟 24pt
- [ ] **Step 4: 烧录验证**：气泡中文正常渲染、流式追加不乱码；生僻字显示豆腐块不崩溃
- [ ] **Step 5: Commit**

```bash
git commit -am "feat: CJK fonts (GB2312 L1, 12/16/24pt) via lv_font_conv

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 16: 屏保

**Files:**
- Modify: `main/app_tasks.c`（屏保任务：待机 60s 无活动 → 屏保态）
- Modify: `components/ui/src/ui_screensaver.c`（新建：暗色时钟 + 亮度 30% + 每 30s 位移 ±2px 防烧屏；任意触摸/按键/唤醒词 → 退出）

**Interfaces:**
- Consumes: 事件组（活动复位 `SCREEN_ACTIVITY_BIT`，任意 UI 交互/对话事件置位）；`board_display_set_brightness()`（board.h 既有或 BSP 接口，以实际为准）
- Produces: 事件位 `SCREENSAVER_ACTIVE_BIT`

- [ ] **Step 1: 屏保 UI**：全屏黑底 + 大时钟（24pt 字体）+ 日期；时钟用现有 SNTP 时间（Ver2.0 ui_main.c 时钟代码复用）
- [ ] **Step 2: 活动计时**：屏保任务每 5s 轮询 `xEventGroupGetBits & SCREEN_ACTIVITY_BIT`，60s 未置位 → 进屏保；屏保中收到任意活动位 → 退出并回主屏
- [ ] **Step 3: 防烧屏位移**：每 30s 时钟对象整体 ±2px 移动（四角循环）
- [ ] **Step 4: 烧录验证**：60s 静置进屏保、亮度下降；点击退出；2 小时运行无烧屏迹象（短时验证亮度/位移逻辑即可）
- [ ] **Step 5: Commit**

```bash
git commit -am "feat: AMOLED screensaver with clock, dimming and pixel shift

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 17: settings 字段扩展 + webserver 豆包配置页

**Files:**
- Modify: `components/settings/include/settings.h` + `settings.c`：新增字段

```c
char api_key[64];        // secret 字段：JSON 导出打码 "****"，导入跳过空值
char voice[48];          // 默认 "zh_female_vv_jupiter_bigtts"
int8_t speed;            // 默认 0
int8_t loudness;         // 默认 0
char system_prompt[512]; // 默认 "你是一个友好的桌面语音助手，回答简洁明了。"
bool auto_continue;      // 默认 true
uint8_t idle_timeout_s;  // 默认 8
```

- Modify: `components/webserver/webserver.c`（新增 `GET/POST /api/doubao`：JSON 读写上述字段；POST 时 api_key 为空不覆盖）
- Modify: `components/webserver/index.html`（"豆包配置"页签：API Key（password input + 显示/隐藏切换）、音色、语速/音量滑条、系统提示词 textarea、自动续听开关；顶部提示"局域网 HTTP 页面，仅建议在可信网络中使用"）
- Modify: `main/app_main.c`（`doubao_init` 的 cfg 改从 settings 读取；settings 变更后重连 doubao——注册 settings 变更回调）

**Interfaces:**
- Consumes: Task 4 `doubao_cfg_t`
- Produces: `/api/doubao` REST 接口（GET 返回 JSON；POST 收同构 JSON）

- [ ] **Step 1: settings 字段 + 默认值注入**（沿用 Ver2.0 `settings_init(defaults)` 模式）
- [ ] **Step 2: webserver 接口**（URI handler 照搬现有 /api/settings 模式）
- [ ] **Step 3: SPA 页签**（照搬现有设置页 JS 结构）
- [ ] **Step 4: 烧录验证**：浏览器访问 `http://<IP>` → 豆包配置页保存 API Key → 设备 `doubao reconnect` 生效；`/api/settings` 导出 JSON 中 api_key 为 "****"
- [ ] **Step 5: Commit**

```bash
git commit -am "feat: doubao settings fields + web config page with masked API key

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## M5：稳定性与交付

### Task 18: 错误处理完善

**Files:**
- Modify: `components/doubao_voice/doubao_voice.c` + `ws_client.c`（错误事件分类）
- Modify: `main/doubao_chat.c`（冷却与恢复）
- Modify: `main/app_tasks.c`（超时看门狗细化，spec §7 表）

**Interfaces:**
- Consumes: `DOUBAO_EVT_ERROR`、`error_log` 组件
- Produces: 无新接口（行为完善）

- [ ] **Step 1: 错误分类**：`DOUBAO_EVT_ERROR` 携带分类枚举（AUTH=401 类/限流/SERVER/网络）；AUTH → UI 错误胶囊"请检查 API Key" + 重连退避封顶 60s；限流 → 冷却 5s 后自动恢复 + 气泡提示"稍后重试"
- [ ] **Step 1b: 内容安全（FR-21）**：`DOUBAO_EVT_AUDIO_STARTED` 的 tts_type=`audit_content_risky` → 跳过后续 AUDIO_DELTA 播报（audio_out 置丢弃模式直至 AUDIO_DONE）+ 气泡显示"（内容已安全过滤）"
- [ ] **Step 2: 超时看门狗**（app_tasks.c）：THINKING 态 commit 后 15s 无 OUTPUT_* 事件 → 重连；SPEAKING 态 5s 无 AUDIO_DELTA → 重连；LISTENING 态不设下行超时（仅 WS ping/pong + 上行发送失败检测）
- [ ] **Step 3: 音频异常恢复**：`board_audio_record/play` 连续 3 次返回错误 → `board_audio_deinit()` + 重初始化（board.h 若有 deinit，无则重启 codec open 流程）；仍失败 → UI 错误提示不崩溃
- [ ] **Step 4: 断网/限流/鉴权失败三类场景实测**（关闭路由器 / 短时高频对话触发限流 / 故意填错 Key）——每类恢复路径符合 spec §7
- [ ] **Step 5: Commit**

```bash
git commit -am "feat: error classification, cooldown, state-specific timeouts, audio recovery

Co-Authored-By: Claude <noreply@anthropic.com>"
```

### Task 19: 集成测试 + 稳定性 + 文档

**Files:**
- Modify: `main/serial_cmd.c`（完善 CLI：`talk`、`doubao status`（连接态/session.id/队列水位/上行丢帧计数/环高水位）、`say <文本>`、`reboot`；删除 Task 5 的 `proto test`（保留或并入 status））
- Modify: `README.md`（构建/烧录/配网/网页配置/串口命令/常见故障）
- Create: `PRD/Project_Resources/doubao-integration-test.md`（验收清单与实测结果记录）

**Interfaces:**
- Consumes: 全部既有接口
- Produces: 交付文档

- [ ] **Step 1: CLI 完善**（`doubao status` 输出上文括号内全部指标——队列水位指标来自 Task 8/10 的计数变量，需暴露 getter）
- [ ] **Step 2: 集成测试执行**（对照 spec §8 全清单 + 验收标准表）：
  1. 启动序列日志验收
  2. 单轮对话 / 连续多轮（10 轮上下文内回答一致）
  3. 打断（说话打断 + 双击打断 + 连续 5 次）
  4. 高音量播报不误触发打断
  5. 清空对话 / 断网重连（含 session 续接与失败新建两条路径）
  6. 唤醒词 / 触摸三手势 / 屏保 / 设置页 / 网页配置
  7. 2 小时连续运行：mem_monitor 内存曲线平稳、各任务栈高水位 < 90%、无 panic（复位原因日志确认无 ESP_RST_WDT/PANIC）
- [ ] **Step 3: 记录实测结果到 doubao-integration-test.md**（含失败项与修复记录）
- [ ] **Step 4: README 与故障排查**（含 API Key 获取路径、音色列表链接、常见错误码表——以实测补充 spec §4.4 的错误码）
- [ ] **Step 5: 最终 Commit**

```bash
git commit -am "docs: integration test report, README, CLI completion

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

## 依赖关系

```
1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 9 → 10 → 11 → 12 → 13 → 14 → 15 → 16 → 17 → 18 → 19
                                          （15/16/17 可与 13/14 并行；17 依赖 4）
```

- Task 1-3（M1）：硬件在手的执行者必须先跑通；无硬件时 1 必须做，2/3 标记待硬件验证
- Task 4-7（M2）：需要真实豆包 API Key（通过 secrets.h 临时注入，禁止 commit）
- Task 8-12（M3）：核心链路，每步硬件实测
- Task 13-17（M4）：UI 可并行推进（不同文件域）
- Task 18-19（M5）：收尾
