# 嵌入式软件开发规范 (CODING STANDARD) v2.0

为了保证 HeyClawy 终端代码的可维护性、稳定性和团队协作效率，所有提交到本仓库的 C 语言代码必须遵循以下开发规范。

## 1. 命名规范

### 1.1 文件与目录
- **文件名**：全部使用小写字母，单词间用下划线分隔，如 `voice_chat.c`, `wifi_manager.h`。
- **目录名**：全小写，简明扼要，如 `main/`, `components/board/`。

### 1.2 变量与宏
- **全局变量**：必须添加 `g_` 前缀，如 `g_app_events`。尽量减少全局变量的使用。
- **静态变量 (模块内全局)**：必须添加 `s_` 前缀，如 `s_lcd_panel`。
- **局部变量**：使用小写加下划线，如 `buffer_size`。
- **宏定义与枚举**：全部使用大写字母，单词间用下划线分隔，并带上模块前缀，如 `BOARD_LCD_H_RES`，`UI_STATE_IDLE`。

### 1.3 函数
- **公共函数**：使用 `模块名_动词_名词` 格式，如 `board_audio_init()`, `ui_set_state()`。
- **私有/静态函数**：无需模块前缀，或者以模块前缀开头，但必须声明为 `static`，如 `static void handle_webserver_toggle(void)`。
- **回调函数**：以 `on_` 开头或以 `_cb` 结尾，如 `on_wifi_state()`, `mp3_player_set_completion_cb()`。

## 2. 架构与状态管理

### 2.1 状态机设计
- 整个设备运行在一个统一的状态机下，状态定义在 `components/ui/ui.h` (如 `UI_STATE_IDLE`, `UI_STATE_LISTENING`)。
- **严禁**在底层或业务逻辑中直接修改 UI 元素的属性（如直接调用 `lv_label_set_text`）。
- 状态切换必须通过调用 `ui_set_state(state)` 完成，由 UI 任务根据状态变化集中更新界面。

#### 2.1.1 状态机最小集合（必须可视化）
- 设备侧状态至少包含：`SLEEP / ARMED / IDLE / LISTENING / THINKING / SPEAKING / PLAYING_MP3 / ERROR`。
- OpenClaw 侧处理阶段（例如 chat delta/final）必须映射到设备侧状态，但不得破坏设备侧互斥约束（例如 SPEAKING 时不得进入 PLAYING_MP3）。

### 2.2 任务间通信
- 任务间同步应使用 `FreeRTOS EventGroup`（本项目统一使用 `g_app_events`）。
- 任务间数据传递应使用 `Queue`（队列）或 `RingBuffer`，避免直接通过全局变量无锁传递数据。

### 2.3 UI 事件解耦
- `components/` 不得直接依赖 `main/`。
- UI → Main 的交互必须通过事件组/队列上报（Setter pattern），并确保 event bit 定义在 UI 与 Main 侧保持一致。

## 3. 并发与资源锁 (CRITICAL)

### 3.1 LVGL 线程安全
LVGL 本身**不是**线程安全的。所有在非 LVGL 渲染任务中对 UI 进行的操作，**必须**使用互斥锁包裹：
```c
if (lvgl_port_lock(pdMS_TO_TICKS(100))) {
    // 进行 LVGL API 调用
    lv_label_set_text(label, "Hello");
    lvgl_port_unlock();
} else {
    ESP_LOGE(TAG, "Failed to get LVGL lock");
}