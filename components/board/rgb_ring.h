/**
 * @file rgb_ring.h
 * @brief RGB LED 环组件 - WS2812B 驱动
 */

#ifndef RGB_RING_H
#define RGB_RING_H

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RGB_MODE_BREATH,
    RGB_MODE_RAINBOW,
    RGB_MODE_AUDIO_FOLLOW,
    RGB_MODE_STATIC,
} rgb_mode_t;

/**
 * @brief 创建 RGB LED 环
 * @param led_count LED 数量
 * @return RGB 环对象
 */
lv_obj_t * rgb_ring_create(uint8_t led_count);

/**
 * @brief 设置 LED 模式
 * @param ring RGB 环对象
 * @param mode 模式
 */
void rgb_ring_set_mode(lv_obj_t *ring, rgb_mode_t mode);

/**
 * @brief 设置单颗 LED 颜色
 * @param ring RGB 环对象
 * @param index LED 索引
 * @param color 颜色
 */
void rgb_ring_set_color(lv_obj_t *ring, uint8_t index, lv_color_t color);

/**
 * @brief 设置所有 LED 颜色
 * @param ring RGB 环对象
 * @param color 颜色
 */
void rgb_ring_set_all_color(lv_obj_t *ring, lv_color_t color);

/**
 * @brief 启动呼吸模式
 * @param ring RGB 环对象
 * @param color 呼吸颜色
 */
void rgb_ring_start_breath(lv_obj_t *ring, lv_color_t color);

/**
 * @brief 停止 LED 动画
 * @param ring RGB 环对象
 */
void rgb_ring_stop(lv_obj_t *ring);

/**
 * @brief 删除 RGB 环
 * @param ring RGB 环对象
 */
void rgb_ring_delete(lv_obj_t *ring);

#ifdef __cplusplus
}
#endif

#endif /* RGB_RING_H */
