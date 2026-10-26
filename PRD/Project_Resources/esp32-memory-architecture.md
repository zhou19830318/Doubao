# ESP32-S3 Memory Architecture for HeyClawy

## Overview
The SenseCAP Watcher (ESP32-S3) has two types of RAM with very different capabilities.
Understanding this is critical to avoid crashes.

## Memory Types

### Internal SRAM (~264KB usable)
- DMA-capable (required for SPI, I2S, etc.)
- Fast access
- Used by default for `malloc()`, task stacks, `xTaskCreate` stacks
- After boot with all peripherals: ~100-150KB free
- Reserve at boot: `esp_psram: Reserving pool of 32K of internal memory for DMA/internal allocations`

### PSRAM (8MB Octal)
- NOT DMA-capable (requires bounce buffers)
- Slower but vastly larger
- Accessed via `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`
- Added to heap allocator at boot
- Good for: large buffers, task stacks that don't need DMA, audio/image data

## Critical Rules

### 1. LCD SPI Driver Needs 32KB DMA Buffers
The SPD2010 display uses SPI with DMA. Each `panel_io_spi_tx_color` call allocates a 32KB
DMA buffer in `setup_priv_desc` (`spi_master.c`). If internal RAM is exhausted, this causes:
```
Mem alloc fail. size 0x00008000 caps 0x00000008
panic_abort → heap_caps_alloc_failed → setup_priv_desc → spi_device_queue_trans
```

### 2. Task Stacks Consume Internal RAM by Default
`xTaskCreatePinnedToCore()` allocates stack in internal RAM. A 16KB stack = 16KB less DMA memory.
Multiple large tasks can exhaust internal RAM.

**Solution**: Use static tasks with PSRAM-allocated stacks:
```c
StaticTask_t *tcb = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
StackType_t *stack = heap_caps_calloc(1, stack_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
xTaskCreateStaticPinnedToCore(func, "name", stack_size, NULL, prio, stack, tcb, core);
```

### 3. FreeRTOS TCB Must Be in Internal RAM
`xTaskCreateStaticPinnedToCore` asserts `xPortCheckValidTCBMem(pxTaskBuffer)`. If TCB is in PSRAM,
you get: `assert failed: xTaskCreateStaticPinnedToCore freertos_tasks_c_additions.h:300`

### 4. LVGL Draw Buffers Should Be Internal RAM
LVGL display buffers need DMA for SPI transactions. Using PSRAM causes OOM for DMA bounce buffers.
Current config: 40-row single buffer in internal RAM.

### 5. minimp3 Decoder Needs >8KB Stack
`mp3dec_decode_frame()` uses ~8-10KB of stack for IMDCT tables and synthesis buffers.
With 8KB stack → DoubleException (stack overflow). Use 16KB minimum.

## Memory Budget (approximate)
| Consumer | Internal RAM | PSRAM |
|---|---|---|
| FreeRTOS kernel | ~20KB | - |
| WiFi + TCP/IP | ~40KB | - |
| LVGL draw buffer (40 rows) | ~33KB | - |
| LCD SPI DMA (dynamic) | ~32KB per flush | - |
| Task stacks (small tasks) | ~16KB (4 × 4KB) | - |
| TTS task stack | - | 16KB ✓ |
| TTS mp3dec_t struct | - | 12KB ✓ |
| TTS MP3 download buffer | - | up to 512KB ✓ |
| TTS PCM buffers | - | variable ✓ |
| WebSocket buffers | ~8KB | - |
| **Available** | **~50-80KB** | **~7.5MB** |

## Diagnostic Commands
```c
// Check free internal RAM
ESP_LOGI(TAG, "Free internal: %d", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
ESP_LOGI(TAG, "Free DMA: %d", heap_caps_get_free_size(MALLOC_CAP_DMA));
ESP_LOGI(TAG, "Free PSRAM: %d", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
ESP_LOGI(TAG, "Largest free DMA block: %d", heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
```
