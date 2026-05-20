/**
 * @file rgb_ring.c
 * @brief RGB LED 环实现
 */

#include "rgb_ring.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "rgb_ring";

typedef struct {
    uint8_t led_count;
    rgb_mode_t mode;
    lv_color_t *colors;
    bool running;
} rgb_ring_data_t;

lv_obj_t * rgb_ring_create(uint8_t led_count)
{
    lv_obj_t *container = lv_obj_create(NULL);
    if (!container) {
        ESP_LOGE(TAG, "Failed to create container");
        return NULL;
    }

    lv_obj_set_user_data(container, calloc(1, sizeof(rgb_ring_data_t)));
    rgb_ring_data_t *data = (rgb_ring_data_t *)lv_obj_get_user_data(container);
    if (!data) {
        lv_obj_delete(container);
        return NULL;
    }

    data->led_count = led_count;
    data->mode = RGB_MODE_STATIC;
    data->colors = calloc(led_count, sizeof(lv_color_t));
    data->running = false;

    ESP_LOGD(TAG, "RGB ring created: %d LEDs", led_count);
    return container;
}

void rgb_ring_set_mode(lv_obj_t *ring, rgb_mode_t mode)
{
    if (!ring) return;

    rgb_ring_data_t *data = (rgb_ring_data_t *)lv_obj_get_user_data(ring);
    if (data) {
        data->mode = mode;
    }
}

void rgb_ring_set_color(lv_obj_t *ring, uint8_t index, lv_color_t color)
{
    if (!ring) return;

    rgb_ring_data_t *data = (rgb_ring_data_t *)lv_obj_get_user_data(ring);
    if (data && index < data->led_count) {
        data->colors[index] = color;
    }
}

void rgb_ring_set_all_color(lv_obj_t *ring, lv_color_t color)
{
    if (!ring) return;

    rgb_ring_data_t *data = (rgb_ring_data_t *)lv_obj_get_user_data(ring);
    if (data) {
        for (int i = 0; i < data->led_count; i++) {
            data->colors[i] = color;
        }
    }
}

void rgb_ring_start_breath(lv_obj_t *ring, lv_color_t color)
{
    if (!ring) return;

    rgb_ring_data_t *data = (rgb_ring_data_t *)lv_obj_get_user_data(ring);
    if (data) {
        data->mode = RGB_MODE_BREATH;
        data->running = true;
    }
}

void rgb_ring_stop(lv_obj_t *ring)
{
    if (!ring) return;

    rgb_ring_data_t *data = (rgb_ring_data_t *)lv_obj_get_user_data(ring);
    if (data) {
        data->running = false;
    }
}

void rgb_ring_delete(lv_obj_t *ring)
{
    if (!ring) return;

    rgb_ring_data_t *data = (rgb_ring_data_t *)lv_obj_get_user_data(ring);
    if (data) {
        if (data->colors) free(data->colors);
        free(data);
    }

    lv_obj_delete(ring);
    ESP_LOGD(TAG, "RGB ring deleted");
}
