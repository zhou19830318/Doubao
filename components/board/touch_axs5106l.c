/**
 * @file touch_axs5106l.c
 * @brief AXS5106L I2C capacitive touch driver + LVGL 9.2 input device
 *
 * Ported from Waveshare ESP32-S3-AUDIO-Board-Demo.
 */

#include "touch_axs5106l.h"
#include "board.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch_axs5106l";

/* Internal state */
static touch_axs5106l_config_t s_cfg;
static lv_indev_t *s_indev = NULL;
static i2c_master_dev_handle_t s_i2c_dev = NULL;

static esp_err_t touch_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    if (!s_i2c_dev) return ESP_ERR_INVALID_STATE;
    
    /* Some AXS chips prefer separate transactions (Stop then Start) over Restart */
    esp_err_t ret = i2c_master_transmit(s_i2c_dev, &reg, 1, pdMS_TO_TICKS(50));
    if (ret != ESP_OK) return ret;
    
    return i2c_master_receive(s_i2c_dev, data, len, pdMS_TO_TICKS(50));
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    /* AXS5106L Data Format (Register 0x01 or 0x00):
     * Some versions use 0x00 as start, some 0x01. 
     * We read 14 bytes from 0x00 to be safe.
     */
    uint8_t buf[16] = {0};
    esp_err_t ret = touch_read_reg(0x00, buf, 16);
    
    bool pressed = false;
    uint16_t x = 0, y = 0;

    if (ret == ESP_OK) {
        /* Heartbeat log to confirm callback is running (debug only) */
        static uint32_t last_tick = 0;
        if (lv_tick_elaps(last_tick) > 10000) {
            ESP_LOGD(TAG, "Touch polling: %02x %02x %02x %02x %02x %02x %02x %02x", 
                     buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
            last_tick = lv_tick_get();
        }

        /* Points is usually at offset 2 if reading from 0x00 (reg 0x02), 
         * or offset 1 if reading from 0x01 (reg 0x02).
         * VoiceClaw_Ver0.4 says reg 0x02 is points.
         */
        uint8_t points = 0;
        uint16_t rx = 0, ry = 0;

        if ((buf[2] & 0x0F) > 0 && (buf[2] & 0x0F) <= 10) {
            // Case 1: Register 0x02 is points (read from 0x00)
            points = buf[2] & 0x0F;
            rx = ((uint16_t)(buf[3] & 0x0F) << 8) | buf[4];
            ry = ((uint16_t)(buf[5] & 0x0F) << 8) | buf[6];
        } else if ((buf[1] & 0x0F) > 0 && (buf[1] & 0x0F) <= 10) {
            // Case 2: Register 0x01 is points (read from 0x00)
            points = buf[1] & 0x0F;
            rx = ((uint16_t)(buf[2] & 0x0F) << 8) | buf[3];
            ry = ((uint16_t)(buf[4] & 0x0F) << 8) | buf[5];
        } else if ((buf[0] & 0x0F) > 0 && (buf[0] & 0x0F) <= 10) {
            // Case 3: Register 0x00 is points (read from 0x00)
            points = buf[0] & 0x0F;
            rx = ((uint16_t)(buf[1] & 0x0F) << 8) | buf[2];
            ry = ((uint16_t)(buf[3] & 0x0F) << 8) | buf[4];
        }

        if (points > 0) {
            x = rx; y = ry;
            pressed = true;
            
            static uint32_t last_raw_log = 0;
            if (lv_tick_elaps(last_raw_log) > 1000) {
                ESP_LOGI(TAG, "Touch detected: points=%d -> rx=%u, ry=%u", points, rx, ry);
                last_raw_log = lv_tick_get();
            }
        }
    } else {
        static uint32_t last_err = 0;
        if (lv_tick_elaps(last_err) > 2000) {
            ESP_LOGW(TAG, "I2C read failed: %s", esp_err_to_name(ret));
            last_err = lv_tick_get();
        }
    }

    /* Debounce release (150ms) */
    static uint32_t release_start_ms = 0;
    static bool last_pressed = false;
    static uint16_t last_x = 0, last_y = 0;
    uint32_t now = lv_tick_get();

    if (pressed) {
        release_start_ms = 0;
        last_x = x;
        last_y = y;
        if (!last_pressed) {
            ESP_LOGI(TAG, "Touch pressed: %u,%u", x, y);
            last_pressed = true;
        }
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        if (last_pressed) {
            if (release_start_ms == 0) release_start_ms = now;
            if (lv_tick_elaps(release_start_ms) < 150) {
                /* Still in debounce window */
                data->state = LV_INDEV_STATE_PRESSED;
            } else {
                ESP_LOGI(TAG, "Touch released");
                last_pressed = false;
                release_start_ms = 0;
                data->state = LV_INDEV_STATE_RELEASED;
            }
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    }

    /* Apply rotation/mirroring if pressed or in debounce */
    if (data->state == LV_INDEV_STATE_PRESSED) {
        uint16_t final_x = last_x;
        uint16_t final_y = last_y;
        if (s_cfg.mirror_x) final_x = s_cfg.x_max - final_x;
        if (s_cfg.mirror_y) final_y = s_cfg.y_max - final_y;
        if (s_cfg.swap_xy) {
            uint16_t tmp = final_x;
            final_x = final_y;
            final_y = tmp;
        }
        data->point.x = final_x;
        data->point.y = final_y;
    }
}

esp_err_t touch_axs5106l_init(const touch_axs5106l_config_t *cfg)
{
    if (cfg) s_cfg = *cfg;
    else {
        touch_axs5106l_config_t def = TOUCH_AXS5106L_CONFIG_DEFAULT();
        s_cfg = def;
    }

    /* Create I2C device on the board's I2C0 bus */
    i2c_master_bus_handle_t bus = board_get_i2c0_handle();
    if (!bus) {
        ESP_LOGE(TAG, "I2C0 bus not initialized on board");
        return ESP_FAIL;
    }

    /* Probe AXS5106L addresses (0x63, 0x5C, 0x3B) */
    uint8_t addrs[] = {0x63, 0x5C, 0x3B};
    esp_err_t ret = ESP_FAIL;
    
    for (int i = 0; i < sizeof(addrs); i++) {
        s_cfg.i2c_addr = addrs[i];
        
        /* Reset touch panel before each probe */
        board_tp_reset(true); // Assert reset
        vTaskDelay(pdMS_TO_TICKS(20));
        board_tp_reset(false); // De-assert reset
        vTaskDelay(pdMS_TO_TICKS(150));

        if (i2c_master_probe(bus, s_cfg.i2c_addr, pdMS_TO_TICKS(100)) == ESP_OK) {
            ESP_LOGI(TAG, "AXS5106L found at 0x%02X", s_cfg.i2c_addr);
            
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = s_cfg.i2c_addr,
                .scl_speed_hz = 100000,
            };
            if (i2c_master_bus_add_device(bus, &dev_cfg, &s_i2c_dev) == ESP_OK) {
                ret = ESP_OK;
                break;
            }
        }
        ESP_LOGW(TAG, "Probe failed at 0x%02X", s_cfg.i2c_addr);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No touch device found on I2C bus");
        return ret;
    }

    ESP_LOGI(TAG, "AXS5106L Touch hardware initialized at 0x%02X", s_cfg.i2c_addr);
    return ESP_OK;
}

esp_err_t touch_axs5106l_register_lvgl(void)
{
    if (!s_i2c_dev) return ESP_ERR_INVALID_STATE;

    /* Register LVGL input device (LVGL 8.3 style) */
    if (!lvgl_port_lock(100)) return ESP_ERR_TIMEOUT;

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    indev_drv.disp = board_get_lvgl_disp();
    
    s_indev = lv_indev_drv_register(&indev_drv);

    lvgl_port_unlock();
    if (!s_indev) {
        ESP_LOGE(TAG, "LVGL indev add failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AXS5106L Touch registered to LVGL (%dx%d)", s_cfg.x_max+1, s_cfg.y_max+1);
    return ESP_OK;
}
