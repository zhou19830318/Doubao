/**
 * @file ui_colors.h
 * @brief 统一颜色定义 - iOS 风格配色系统
 */

#ifndef UI_COLORS_H
#define UI_COLORS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ════════════════════════════════════════════════════════════════════════
 * 主色调 (Primary Colors)
 * ════════════════════════════════════════════════════════════════════════ */

#define UI_COLOR_PRIMARY       0x007AFF  /* iOS 蓝色 */
#define UI_COLOR_SUCCESS       0x30D158  /* iOS 绿色 */
#define UI_COLOR_PURPLE        0xAF52DE  /* iOS 紫色 */
#define UI_COLOR_WARNING       0xFF9500  /* iOS 橙色 */
#define UI_COLOR_ERROR         0xFF3B30  /* iOS 红色 */

/* ════════════════════════════════════════════════════════════════════════
 * 背景色 (Background Colors)
 * ════════════════════════════════════════════════════════════════════════ */

#define UI_COLOR_BG_MAIN       0xFFFFFF  /* 主背景 - 白色 */
#define UI_COLOR_BG_SECONDARY  0xF2F2F7  /* 次要背景 - 灰白 */
#define UI_COLOR_BG_TERTIARY   0xE5E5EA  /* 第三背景 - 灰色 */
#define UI_COLOR_BUBBLE        0xF2F2F7  /* 聊天气泡背景 */

/* ════════════════════════════════════════════════════════════════════════
 * 文字颜色 (Text Colors)
 * ════════════════════════════════════════════════════════════════════════ */

#define UI_COLOR_TEXT_PRIMARY   0x000000  /* 主要文字 - 黑色 */
#define UI_COLOR_TEXT_SECONDARY 0x666666  /* 次要文字 - 灰色 */
#define UI_COLOR_TEXT_TERTIARY  0x999999  /* 辅助文字 - 浅灰 */
#define UI_COLOR_TEXT_INVERSE   0xFFFFFF  /* 反白文字 */

/* ════════════════════════════════════════════════════════════════════════
 * 液态球颜色 (Liquid Sphere - iOS 绿色风格)
 * ════════════════════════════════════════════════════════════════════════ */

#define UI_COLOR_LIQUID_CENTER  0x66D478  /* 中心 - 浅绿 */
#define UI_COLOR_LIQUID_EDGE    0x30D158  /* 边缘 - iOS 绿 */

/* ════════════════════════════════════════════════════════════════════════
 * 声波颜色 (Waveform Colors)
 * ════════════════════════════════════════════════════════════════════════ */

#define UI_COLOR_WAVEFORM_IDLE     0x6366F1  /* 待机 - 靛蓝 */
#define UI_COLOR_WAVEFORM_LISTEN   0x30D158  /* 聆听中 - 绿色 */
#define UI_COLOR_WAVEFORM_SPEAKING 0x007AFF  /* 说话中 - 蓝色 */

/* ════════════════════════════════════════════════════════════════════════
 * 状态指示器颜色
 * ════════════════════════════════════════════════════════════════════════ */

#define UI_COLOR_STATUS_CONNECTED   UI_COLOR_SUCCESS
#define UI_COLOR_STATUS_CONNECTING  UI_COLOR_WARNING
#define UI_COLOR_STATUS_DISCONNECTED UI_COLOR_ERROR
#define UI_COLOR_STATUS_LISTENING    UI_COLOR_PRIMARY

/* ════════════════════════════════════════════════════════════════════════
 * 便捷宏
 * ════════════════════════════════════════════════════════════════════════ */

#define UI_COLOR_PRIMARY_LV       lv_color_hex(UI_COLOR_PRIMARY)
#define UI_COLOR_SUCCESS_LV        lv_color_hex(UI_COLOR_SUCCESS)
#define UI_COLOR_PURPLE_LV         lv_color_hex(UI_COLOR_PURPLE)
#define UI_COLOR_LIQUID_CENTER_LV  lv_color_hex(UI_COLOR_LIQUID_CENTER)
#define UI_COLOR_LIQUID_EDGE_LV    lv_color_hex(UI_COLOR_LIQUID_EDGE)
#define UI_COLOR_BUBBLE_LV         lv_color_hex(UI_COLOR_BUBBLE)

#ifdef __cplusplus
}
#endif

#endif /* UI_COLORS_H */