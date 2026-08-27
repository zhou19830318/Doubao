# ESP32-S3 AI 语音机器人代码 Review 报告

## 审查范围

- 状态机与状态同步
- FreeRTOS 任务、栈和任务退出
- 堆、PSRAM、DMA 缓冲区生命周期
- 音频采集、播放、WebSocket 并发
- LVGL UI 线程安全、状态切换和刷新性能
- 卡死、死锁、资源竞争和恢复路径

审查基于源码静态分析完成，工程源码未修改。

---

## 一、总体结论

当前工程已经针对不少现场问题做过修复，例如：

- 唤醒词和语音采集任务避免同时读取 I2S。
- WebSocket 回调不再直接执行复杂 teardown。
- MP3 播放完成回调通过事件队列延迟处理 UI。
- TTS 音频支持 drain，避免 `audio.done` 后尾音被截断。
- 大型缓冲区主要放入 PSRAM。
- MP3 UI 已避免使用高成本 transform 动画。

但是当前仍存在几处必须优先处理的问题：

| 等级 | 数量 | 主要影响 |
|---|---:|---|
| Critical | 4 | 死锁、Use-after-free、并发环形缓冲损坏、硬件重配置竞态 |
| High | 8 | 状态卡死、任务生命周期不确定、栈/堆风险、UI 线程违规 |
| Medium | 9 | 状态机保护失效、UI 卡顿、内存碎片、错误恢复不完整 |
| Low | 若干 | 可维护性、诊断能力和接口一致性 |

当前版本还不能认为已经满足“状态机不会卡死、栈内存可靠释放、UI 始终流畅”的要求。

---

# 二、Critical 问题

## C1. 状态机存在确定性的递归加锁死锁

**文件：**

```text
main/app_state_machine.c:191-225
main/app_state_machine.c:275-289
```

问题链路：

```c
app_state_request()
    -> xSemaphoreTake(s_state_mutex)
    -> app_state_force_idle(owner)
        -> app_state_release(owner)
            -> xSemaphoreTake(s_state_mutex)
```

`app_state_request()` 已经持有 `s_state_mutex`，随后在资源抢占路径调用 `app_state_force_idle()`，而 `app_state_force_idle()` 又调用 `app_state_release()`，后者再次获取同一个普通 mutex。

这会导致任务永久阻塞。

触发后可能表现为：

- 按键无响应
- UI 停在当前状态
- 音频任务无法继续调度
- 看门狗复位
- 设备表现为偶发死机

### 建议修复

禁止在持有状态机锁时调用任何公开状态机 API。

改为拆分为内部函数：

```c
static void state_release_resources_locked(ui_state_t state);
static void state_force_idle_locked(ui_state_t state);
```

或者采用两阶段流程：

1. 在锁内只记录需要被抢占的 owner。
2. 释放锁。
3. 执行停止音频、停止任务等外部操作。
4. 重新获取锁提交状态变化。

---

## C2. 状态机锁内执行阻塞操作，存在死锁和长时间阻塞风险

**文件：**

```text
main/app_state_machine.c:234-250
main/app_state_machine.c:261-271
```

以下函数在持有 `s_state_mutex` 时执行：

```c
on_leave_state(old);
on_enter_state(target);
```

而 enter/leave hook 可能调用：

```c
wake_word_pause();
wake_word_resume();
```

`wake_word_pause()` 内部最多等待 500 ms。状态机 mutex 可能被持有 500 ms 甚至更久。若音频任务反向访问状态机，还可能形成锁顺序死锁。

### 建议

状态转换锁内只能执行：

- 校验
- 状态变量写入
- 资源 owner 记录修改

不能执行：

- I2S 操作
- 音频停止/启动
- WebSocket 操作
- LVGL 操作
- `vTaskDelay`
- 可能获取其他 mutex 的函数

将 enter/leave hook 改为解锁后异步执行，或者放入统一的 app state worker task。

---

## C3. 音频输出环形缓冲的读写锁保护不完整

**文件：**

```text
components/doubao_voice/audio_out.c:43-50
components/doubao_voice/audio_out.c:93-116
components/doubao_voice/audio_out.c:326-360
```

生产者 `dbaudio_out_push()` 会获取 `s_ring_mutex`，但是消费者 `ring_pop_block()` 没有获取同一个 mutex。同时 `dbaudio_out_start()`、`dbaudio_out_interrupt()` 也可能修改 ring 状态。

共享变量包括：

```c
s_ring_head
s_ring_tail
s_ring_count
```

`volatile` 不能解决多字段操作原子性、读写时序和 reset/pop 竞态问题。

可能表现为：

- `s_ring_count` 异常
- 播放任务读取未完成写入的数据
- 环形缓冲跳读或重复播放
- 长时间静音
- 播放任务无法退出

### 建议

优先使用 FreeRTOS queue 或 stream buffer 替代手写 ring。

如果保留 ring：

- 所有 head/tail/count 修改统一在同一个 mutex 内。
- 或改为严格的单生产者/单消费者无锁 ring。
- reset 必须通过事件通知 playback task 执行，禁止外部直接清空。

同时增加：

```c
if (samples > RING_CAP_SAMPLES) {
    return ESP_ERR_INVALID_SIZE;
}
```

---

## C4. 唤醒词任务停止时可能提前释放模型，存在 Use-after-free

**文件：**

```text
components/wake_word/wake_word.c:247-276
```

当前停止逻辑只等待固定的 200 ms，没有确认任务已经退出。任务可能仍卡在 `board_audio_record()` 或执行模型 detect，随后主线程就销毁模型内存。

### 建议

增加退出确认信号量：

```c
static SemaphoreHandle_t s_exit_ack;
```

任务退出前：

```c
xSemaphoreGive(s_exit_ack);
s_task_handle = NULL;
vTaskDelete(NULL);
```

停止流程必须等待退出确认，确认成功后才能销毁模型。超时后不能继续销毁模型，应进入安全降级或重启流程。

---

# 三、High 问题

## H1. `s_cap_running` 设置时序可能导致采集任务状态错误

**文件：**

```text
components/doubao_voice/audio_in.c:151-168
```

当前是创建任务之后才设置 `s_cap_running = true`。如果任务快速因 DMA buffer 分配失败而退出，任务入口可能先把标志置为 false，创建函数随后又写回 true，造成状态错误。

### 建议

使用任务启动握手，或只在任务入口设置运行状态，并由任务出口清零。

---

## H2. `dbaudio_in_stop()` 超时后仍返回 `ESP_OK`

**文件：**

```text
components/doubao_voice/audio_in.c:171-179
```

即使 200 ms 后任务仍在运行，也返回成功。上层可能立即重配置 I2S，形成硬件访问竞态。

### 建议

```c
if (s_cap_running) {
    ESP_LOGE(TAG, "capture task stop timeout");
    return ESP_ERR_TIMEOUT;
}
return ESP_OK;
```

---

## H3. `dbaudio_out_stop()` 具有同样的超时误报问题

**文件：**

```text
components/doubao_voice/audio_out.c:376-385
```

播放任务可能阻塞在 `board_audio_play()`，但停止函数等待 200 ms 后仍然返回成功。

### 建议

增加播放任务退出确认。超时应返回 `ESP_ERR_TIMEOUT`，并禁止后续立即进行硬件重配置。

---

## H4. `go_idle()` 在 WebSocket/dbws 任务中执行大量阻塞操作

**文件：**

```text
main/doubao_chat.c:327-357
main/doubao_chat.c:375-420
main/doubao_chat.c:491-501
main/doubao_chat.c:625-638
```

`doubao_chat_on_event()` 在 dbws 任务上下文中执行，但事件处理可能调用：

```c
go_idle()
notes_manager_save_message()
dbaudio_in_stop()
dbaudio_out_stop()
doubao_close_session()
wake_word_resume()
```

这可能导致 RX fragment 堆积、WebSocket 发送延迟、TTS 断流和 session 超时。

### 建议

WebSocket 回调只投递事件：

```c
chat_event_queue_send(CHAT_CMD_GO_IDLE, payload);
```

由独立的 `chat_control_task` 处理状态机、音频任务启停、session 生命周期、SD/NVS 和 UI 请求。

---

## H5. MP3 事件 payload 没有检查长度，可能发生栈覆盖

**文件：**

```text
components/mp3_player/mp3_player.c:102-104
components/mp3_player/mp3_player.c:134-136
```

当前代码直接使用：

```c
memcpy(&info, event->payload, event->payload_size);
memcpy(&st, event->payload, event->payload_size);
```

如果 payload 大于目标结构体大小，会覆盖栈变量及邻近内存。

### 修复方式

```c
if (!event || !event->payload) {
    return -1;
}

if (event->payload_size > sizeof(info)) {
    return -1;
}

memcpy(&info, event->payload, event->payload_size);
```

所有事件 payload 都应执行同样检查。

---

## H6. MP3 扫描把 `int *` 强制转换为 `uint16_t *`

**文件：**

```text
main/app_state.c:151-153
```

当前：

```c
mp3_player_scan_sd_dynamic(
    "/sdcard/mp3",
    &s_sd_mp3_names,
    (uint16_t *)&s_sd_mp3_count
);
```

这会只写入 `int` 的低 16 位，高 16 位保留旧值，可能造成后续循环和释放越界。

### 修复

```c
uint16_t count = 0;
mp3_player_scan_sd_dynamic(
    "/sdcard/mp3",
    &s_sd_mp3_names,
    &count
);
s_sd_mp3_count = (int)count;
```

---

## H7. 唤醒词任务实际使用内部栈，与注释不一致

**文件：**

```text
components/wake_word/wake_word.c:205-206
```

实际使用普通 `xTaskCreatePinnedToCore()`，任务栈默认来自内部 RAM，而注释声称使用 PSRAM。

4 KB 栈对于 ESP-SR、日志、浮点运算和模型调用偏小，必须通过 high-water mark 证明安全。

### 建议

- 改用 `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)`，或
- 提高到 8 KB/12 KB，记录 `uxTaskGetStackHighWaterMark()`。

---

## H8. PSRAM 任务栈大小语义依赖自定义实现，存在维护风险

**文件：**

```text
components/doubao_voice/doubao_task_mem.h:20-56
```

当前通过 `DB_STACK_WORDS(bytes)` 转换栈大小，但工程注释指出 IDF 文档与实现的单位说明存在矛盾。

### 建议

使用明确命名：

```c
DB_TASK_CREATE_PSRAM_WORDS(...)
DB_TASK_CREATE_PSRAM_BYTES(...)
```

同时增加实际分配大小检查，避免升级 ESP-IDF 后出现栈只有预期四分之一或意外分配四倍内存。

---

# 四、状态机设计问题

## S1. 存在两套事实来源，状态机保护已被系统性绕过

主要路径包括：

```text
app_state_request()
app_set_state()
ui_set_state()
s_chat_state
```

大量代码直接使用：

```c
app_set_state(UI_STATE_LISTENING);
app_set_state(UI_STATE_TTS_PLAYING);
app_set_state(UI_STATE_IDLE);
```

当前 `app_set_state()` 会同时修改状态机内部状态、UI 和 LED，但不校验合法转换，也不是原子操作。

### 建议：只保留一个状态所有者

推荐结构：

```text
所有任务/回调
      |
      v
app_state_post(EVENT)
      |
      v
state_controller_task
      |
      +-- 校验状态转换
      +-- 资源仲裁
      +-- 启停音频任务
      +-- 发布 UI 状态事件
      +-- 发布 LED 状态事件
```

业务层不应直接调用 `app_set_state()` 或 `app_state_machine_force_current()`。

---

## S2. 未知状态默认允许转换

**文件：**

```text
main/app_state_machine.c:94-101
```

当前未知状态、未配置状态默认允许转换，新增 enum 状态却未更新转换表时不会暴露错误。

### 建议

改为未知状态默认拒绝，并使用 `STATE_COUNT` 和 `UI_STATE_INVALID` 做边界保护。

---

## S3. `ERROR` 无条件允许进入

**文件：**

```text
main/app_state_machine.c:99
```

可以保留全局错误逃生路径，但应通过明确错误事件进入，而不是允许所有调用方直接跳转。

---

## S4. `app_state_force_idle()` 可能错误覆盖当前 UI

**文件：**

```text
main/app_state_machine.c:275-289
```

即使传入的 state 不是当前状态，也会调用 `app_set_state(UI_STATE_IDLE)`，可能造成 UI、对话和音频实际状态不一致。

### 建议

只有确认传入状态仍是当前状态时，才允许更新全局 UI 状态。

---

# 五、音频和 WebSocket 并发问题

## A1. 输入音频队列的状态读取未统一加锁

**文件：**

```text
components/doubao_voice/audio_in.c:51-60
components/doubao_voice/audio_in.c:218-230
```

`dbaudio_in_queue_depth()` 和 `dbaudio_in_drop_count()` 直接读取同时被不同任务修改的变量。`volatile` 不能保证一致性。

### 建议

使用 mutex、原子计数，或直接替换为 FreeRTOS queue。

---

## A2. 音频队列溢出策略需要分别设计

输入队列和播放队列都采用 drop-oldest，但两者影响不同：

- 输入队列丢弃句首会影响 ASR。
- 播放队列丢弃旧数据会造成 TTS 缺字。

建议对输入、播放和控制消息分别制定策略，控制消息不能被丢弃。

---

## A3. TX 队列满时所有消息统一丢弃不安全

**文件：**

```text
components/doubao_voice/ws_client.c:210-241
```

`session.create`、`session.close`、`response.cancel`、commit 等控制帧不能和音频帧采用同一丢弃策略。

### 建议

分为：

```text
control_tx_queue —— 不丢弃，优先级高
audio_tx_queue   —— 可限流或丢弃
```

---

## A4. `dbws_drain_tx()` 仍可能出现 RX/TX 活锁

**文件：**

```text
components/doubao_voice/ws_client.c:736-766
```

固定处理 8 个 fragment 不能完全保证控制帧及时发送。建议使用处理时间预算、控制帧独立队列和 proto_feed 耗时统计。

---

## A5. 音频流只使用全局 bool，无法区分 response generation

**文件：**

```text
components/doubao_voice/doubao_voice.c:249-255
```

旧响应延迟到达时，单一 `s_audio_stream_open` 无法区分旧响应、新响应和新 session。

### 建议

为每轮响应增加 generation 或 session id，旧 generation 的音频直接丢弃。

---

# 六、UI 线程安全和流畅度

## U1. UI API 的线程约束没有在接口层保证

部分 UI API 不自行加锁，依赖调用方记得获取 LVGL lock。工程演进后容易再次出现随机崩溃、花屏和对象生命周期问题。

### 建议

推荐所有非 LVGL 任务通过 UI command queue 请求更新，由 LVGL task 统一执行。

---

## U2. `ui_bot_bubble_append()` 的实际显示内容可能无限增长

**文件：**

```text
components/ui/src/ui_main.c:1281-1307
```

内部 `s_bot_text` 达到 4096 字节后虽然停止增长，但后续 delta 仍可能继续插入 LVGL label，导致显示内容和内存使用继续增长。

### 建议

达到上限后截断追加，并明确显示“内容过长，已截断”。

---

## U3. 气泡动态创建/删除可能造成碎片和重布局

**文件：**

```text
components/ui/src/ui_main.c:1215-1246
```

建议使用 8～12 个固定气泡对象池，或者只保留最近 6～10 条可视内容。

---

## U4. 波形动画刷新频率偏高

**文件：**

```text
components/ui/src/ui_main.c:219-241
```

40 ms 更新 16 个对象，每秒最多 400 次属性修改。在 LCD SPI DMA 和音频同时运行时可能造成刷新堆积。

### 建议

- 使用 canvas 绘制波形。
- 降低到 60～80 ms。
- 仅更新变化超过阈值的 bar。
- 播放音频时降低 UI 刷新频率。

---

## U5. MP3 UI 与全局手势存在竞争

**文件：**

```text
components/ui/src/ui_mp3_ui.c:201-263
main/app_main.c:650-667
```

当前通过 `s_selection_confirmed` 进行抑制，属于补丁式协调，仍存在事件顺序变化导致误触发的风险。

### 建议

当 MP3 overlay 可见时，由 UI 层统一消费所有触摸事件，不再让全局手势 detector 同时处理。

---

## U6. `ui_tasks` 使用的事件 flag 语义与注释不符

**文件：**

```text
components/ui/ui_tasks.c:118-124
```

`LV_OBJ_FLAG_GESTURE_BUBBLE` 是允许手势传播，不是阻止事件传播。任务 item 点击处理目前也基本被注释掉。

### 建议

明确使用事件冒泡/手势冒泡语义，并实现完整的任务 item callback。

---

# 七、栈和内存生命周期清单

## 任务栈概览

| 任务 | 创建方式 | 栈大小 | 栈区域 | 主要风险 |
|---|---|---:|---|---|
| `doubao_play` | `WithCaps` | 16 KB | PSRAM | 需 high-water 验证 |
| `doubao_cap` | `WithCaps` | 16 KB | PSRAM | stop 无 join |
| `dbws` | `WithCaps` | 12 KB | PSRAM | dbws 内仍执行重活 |
| `send_audio_task` | `WithCaps` | 16 KB | PSRAM | VAD/JSON 路径需测量 |
| `wake_word` | 普通创建 | 4 KB | Internal | 与注释不符，偏小 |
| `knob` | 普通创建 | 4 KB | Internal | 按键逻辑较多 |
| `status` | 普通创建 | 6 KB | Internal | 持有 LVGL 锁，逻辑较重 |
| `sleep` | 普通创建 | 4 KB | Internal | 需检查深睡路径 |
| MP3 pipeline | GMF | 8 KB | PSRAM 配置 | 依赖 `stack_in_ext` 实现 |
| WebSocket internal | 普通创建 | 12 KB | Internal | TLS 和连续消息路径 |
| `serial_cmd` | REPL | 4 KB | Internal | 不应执行大 JSON/SD 操作 |

## 栈内存建议

必须增加统一的任务监控：

```c
uxTaskGetStackHighWaterMark(task_handle)
```

建议记录：

- task name
- 当前剩余栈
- 历史最小剩余栈
- core
- priority
- stack start/end

建议开启：

```text
CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=y
CONFIG_FREERTOS_WATCHPOINT_END_OF_STACK=y
CONFIG_HEAP_TASK_TRACKING=y
```

当前已经启用栈 canary，但 `CONFIG_HEAP_TASK_TRACKING` 未启用，`CONFIG_SPI_FLASH_AUTO_SUSPEND` 也未启用。

工程使用 PSRAM 任务栈，而 NVS/Flash 写操作期间可能关闭 cache。若后续增加 OTA、频繁 NVS 保存或 Flash 擦除，需要重新评估 PSRAM stack 在 cache-off 窗口的安全性。

---

# 八、堆、PSRAM 和 DMA 内存问题

## 当前较合理的设计

- 音频 playback hot-path buffer 使用 `MALLOC_CAP_DMA`。
- 采集临时 buffer 使用 internal DMA RAM。
- 大型音频 ring 使用 PSRAM。
- WebSocket RX fragment ring 使用 PSRAM。
- cJSON hook 指向 PSRAM。
- 长任务栈放 PSRAM。
- 使用 `vTaskDeleteWithCaps()` 释放 PSRAM 任务栈。

## 仍需改进

### 1. 常驻 ring 没有释放接口

输入队列和输出队列只初始化一次，没有 cleanup。若这是常驻缓存，应明确标记并纳入内存统计；若支持模块重启，则必须增加 deinit，并确认任务已经退出后再释放。

### 2. WebSocket RX ring 占用约 1 MB PSRAM

```c
256 × 4200 = 1,075,200 bytes
```

此外还有 TX queue、session buffer、protocol buffer、playback ring 和 resample buffer。建议增加水位、耗时、丢包统计，而不是只扩大 ring。

---

# 九、建议的重构方向

## 第一阶段：先解决稳定性问题

1. 修复状态机递归加锁。
2. 状态机锁内禁止调用 `wake_word_pause/resume`。
3. 给 audio in/out 增加退出确认。
4. `wake_word_stop()` 等待任务退出后再 destroy 模型。
5. 修复 audio out ring 的同步保护。
6. 修复 MP3 payload 长度检查。
7. 修复 `uint16_t *` 强转写入 `int` 的问题。
8. 移除 `app_set_state()` 绕过状态机的双重事实来源。

## 第二阶段：统一状态控制

统一使用 `app_state_post(EVENT)`，由单一 controller task 处理状态转换和资源仲裁。

## 第三阶段：统一 UI 更新

网络、音频、按键任务全部通过 UI command queue 请求更新，由 LVGL task 统一执行。

## 第四阶段：性能优化

- 减少或动态调整音频 ring 容量。
- 控制 TX/RX 优先级。
- 波形改 canvas 或减少刷新频率。
- 气泡改对象池。
- 增加 response generation。
- 增加任务栈和堆实时监控。

---

# 十、建议增加的验证测试

## 1. 状态机压力测试

连续测试：

```text
wake -> cancel
wake -> no speech timeout
wake -> session timeout
wake -> network disconnect
TTS -> button interrupt
TTS -> wake word interrupt
MP3 -> wake word
MP3 -> button stop
```

验证最终是否：

- 回到 IDLE
- 恢复唤醒词
- 采集任务退出
- 播放任务退出
- session 关闭
- UI 与内部状态一致

## 2. 任务生命周期测试

重复 1000 次：

```text
start capture -> stop capture
start playback -> stop playback
start session -> close session
wake_word_pause -> resume
wake_word_stop -> start
```

记录 task count、free heap、free PSRAM、largest free block、stack high-water mark 和 DMA free memory。

## 3. 音频压力测试

测试长文本 TTS、服务端 burst、网络延迟、ring 满、播放 cancel、MP3/TTS 连续切换。

## 4. UI 压力测试

测试 1000 次状态切换、100 条以上长消息、连续流式 delta、MP3 overlay 开关、双击/长按/滑动混合操作，以及音频播放同时运行波形动画。

---

# 十一、当前验证结果

已完成：

- 源码结构审计
- 状态机调用路径审计
- 任务创建和删除路径扫描
- 堆、PSRAM、DMA 分配/释放路径扫描
- LVGL 调用路径搜索
- 音频和 WebSocket 并发路径检查
- 工程配置中的 PSRAM、栈溢出检查和 Flash 设置确认

当前环境限制：

- 未发现 `idf.py`
- 未发现完整 ESP-IDF 工具链
- 未发现 `cppcheck`
- 无法执行真实 `idf.py build`
- 无法执行硬件级 I2S、LVGL、PSRAM 压力测试
- 未获得真实任务 high-water mark 数据

因此报告中的问题分为确定性代码问题、高概率运行时问题和需要硬件验证的问题。

---

## 最终建议

在继续增加功能之前，建议先完成以下 8 项修复：

```text
1. 修复 app_state_request() -> app_state_force_idle() 递归锁死锁
2. 状态机锁内禁止调用 wake_word_pause/resume
3. audio_out ring 读写统一同步
4. audio_in/out stop 增加真正的退出确认
5. wake_word_stop 等待任务退出后再 destroy 模型
6. 所有 MP3 event payload 增加长度检查
7. 修复 uint16_t * 强转写入 int 的问题
8. 移除 app_set_state() 绕过状态机的双重事实来源
```

完成上述修复后，再进行 UI 单线程化、气泡对象池化、波形优化、WebSocket 控制帧分队列以及 response generation 改造。
