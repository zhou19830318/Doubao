/*
 * SPDX-FileCopyrightText: 2024-2026 AIWearable Contributors
 * SPDX-License-Identifier: MIT
 *
 * Memory monitoring utilities for debugging memory issues
 */

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdio.h>

static const char *TAG = "mem_monitor";

void mem_monitor_snapshot(const char *context)
{
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_heap = esp_get_minimum_free_heap_size();
    uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    
    ESP_LOGI(TAG, "Memory snapshot [%s]:", context ? context : "unknown");
    ESP_LOGI(TAG, "  Total free heap: %lu bytes (min: %lu)", 
             (unsigned long)free_heap, (unsigned long)min_heap);
    ESP_LOGI(TAG, "  Free internal RAM: %lu bytes", (unsigned long)free_internal);
    ESP_LOGI(TAG, "  Free PSRAM: %lu bytes", (unsigned long)free_psram);
    
    /* Warning thresholds */
    if (free_heap < 50000) {
        ESP_LOGW(TAG, "  ⚠️  CRITICAL: Very low heap memory!");
    } else if (free_heap < 100000) {
        ESP_LOGW(TAG, "  ⚠️  WARNING: Low heap memory");
    }
    
    if (free_internal < 50000) {
        ESP_LOGW(TAG, "  ⚠️  CRITICAL: Very low internal RAM!");
    } else if (free_internal < 100000) {
        ESP_LOGW(TAG, "  ⚠️  WARNING: Low internal RAM");
    }
}

void mem_monitor_task_stats(void)
{
    TaskStatus_t *task_array;
    UBaseType_t task_count, array_size;
    uint32_t total_runtime;
    
    /* Get number of tasks */
    array_size = uxTaskGetNumberOfTasks();
    task_array = pvPortMalloc(array_size * sizeof(TaskStatus_t));
    
    if (task_array) {
        task_count = uxTaskGetSystemState(task_array, array_size, &total_runtime);
        
        ESP_LOGI(TAG, "Task statistics (%u tasks):", task_count);
        
        for (UBaseType_t i = 0; i < task_count; i++) {
            ESP_LOGI(TAG, "  %-16s Stack HWM: %u, Priority: %u",
                     task_array[i].pcTaskName,
                     task_array[i].usStackHighWaterMark,
                     task_array[i].uxCurrentPriority);
        }
        
        vPortFree(task_array);
    }
}

/* Print heap capabilities summary */
void mem_monitor_caps_summary(void)
{
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "---");
    heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
}