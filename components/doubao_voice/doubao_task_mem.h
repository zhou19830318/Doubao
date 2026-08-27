/* Private helper: create doubao_voice's long-stack tasks with the stack in
 * PSRAM.
 *
 * Why: the four tasks here (dbws / send / cap / play) each want a 16KB
 * stack. On this board that is 64KB of internal DRAM against a ~168KB
 * budget which WiFi alone takes 64KB of — cap and play simply failed to
 * spawn ("Failed to create capture task"), which starved the uplink and
 * got the session killed. PSRAM sits ~4.5MB free, so the stacks go there
 * and the TCBs stay internal (xTaskCreate*WithCaps uses pvPortMalloc for
 * the TCB precisely because FreeRTOS requires that).
 *
 * The flash-erase hazard (CLAUDE.md 铁律 5 — cache off during an erase
 * makes PSRAM unreadable, so a task running off a PSRAM stack faults) is
 * bounded here: this firmware's hot path writes no internal flash. Chat
 * history goes to /sdcard over SPI, and the only internal-flash writer is
 * settings_save() → NVS, which happens on a user-initiated config change.
 * If OTA or a periodic NVS writer is ever added, revisit this — either
 * enable CONFIG_SPI_FLASH_AUTO_SUSPEND or move these stacks back.
 *
 * UNIT TRAP: xTaskCreate*WithCaps() takes the stack depth in WORDS — it
 * allocates usStackDepth * sizeof(StackType_t). The doc comment in
 * idf_additions.h claiming "number of bytes" contradicts the
 * implementation in idf_additions.c; the implementation is what runs.
 * That is the opposite of plain xTaskCreate(), which takes bytes in
 * ESP-IDF. DB_STACK_WORDS() keeps the call sites written in bytes so the
 * numbers stay comparable to the rest of the project.
 *
 * Tasks created this way MUST be torn down with vTaskDeleteWithCaps(),
 * otherwise the PSRAM stack leaks.
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define DB_STACK_WORDS(bytes)  ((bytes) / sizeof(StackType_t))

/* Logs the PSRAM actually consumed alongside the stack size we asked for.
 * If the two disagree by 4x, the words-vs-bytes reading above is wrong and
 * every stack here is either a quarter or quadruple its intended size —
 * which is exactly the kind of thing that hides until a deep call path
 * smashes the stack. Cheap to keep; reads as a one-line assertion at boot. */
static inline BaseType_t db_task_create_psram(TaskFunction_t fn, const char *name,
                                              uint32_t stack_bytes, void *arg,
                                              UBaseType_t prio,
                                              TaskHandle_t *handle, BaseType_t core)
{
    size_t before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        fn, name, DB_STACK_WORDS(stack_bytes), arg, prio, handle, core,
        MALLOC_CAP_SPIRAM);
    if (ret == pdPASS) {
        size_t used = before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI("doubao_task", "%s: stack %ub requested, %ub PSRAM taken, "
                 "internal free %u", name, (unsigned)stack_bytes, (unsigned)used,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    return ret;
}

#define DB_TASK_CREATE_PSRAM(fn, name, stack_bytes, arg, prio, handle, core) \
    db_task_create_psram((fn), (name), (stack_bytes), (arg), (prio),         \
                         (handle), (core))
