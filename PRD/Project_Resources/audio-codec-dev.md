# Audio Codec Dev Framework (ESP32 SenseCAP Watcher)

## ⚠️ CRITICAL: Do NOT use raw I2C register writes for codec initialization

The SenseCAP Watcher uses the `esp_codec_dev` component for audio codec management. Raw I2C register writes will NOT properly initialize the ES7243E microphone codec — the mic will only capture noise (avg_abs=3-4 even with max gain).

## Architecture

```
Audio Pipeline:
  Speaker: ES8311 DAC ← esp_codec_dev_write() ← I2S TX
  Mic:     ES7243E ADC → esp_codec_dev_read() → I2S RX
  Both share the same I2S bus (I2S_NUM_0)
```

## Initialization Flow (matching Seeed BSP)

### 1. Create I2S channels
```c
i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
chan_cfg.auto_clear = true;
chan_cfg.intr_priority = 4;
i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan);
```

### 2. Configure I2S (matching Seeed BSP slot config)
```c
i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
    .slot_cfg = {
        .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
        .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
        .slot_mode = I2S_SLOT_MODE_MONO,
        .slot_mask = I2S_STD_SLOT_BOTH,
        .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
        .ws_pol = false,
        .bit_shift = true,     // Must be true
        .left_align = true,    // Must be true
        .big_endian = false,
        .bit_order_lsb = false,
    },
    .gpio_cfg = { .mclk=10, .bclk=11, .ws=12, .dout=16, .din=15 },
};

// TX (speaker): use default SLOT_BOTH
i2s_channel_init_std_mode(tx_chan, &std_cfg);
i2s_channel_enable(tx_chan);

// RX (mic): MUST use SLOT_RIGHT (mic data is on right I2S slot)
std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
i2s_channel_init_std_mode(rx_chan, &std_cfg);
i2s_channel_enable(rx_chan);
```

### 3. Create I2S data interface
```c
audio_codec_i2s_cfg_t i2s_cfg = {
    .port = I2S_NUM_0,
    .rx_handle = rx_chan,
    .tx_handle = tx_chan,
};
const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
```

### 4. Create speaker codec (ES8311)
```c
audio_codec_i2c_cfg_t spk_i2c = {
    .port = 0,
    .addr = 0x18 << 1,  // 7-bit addr 0x18, must left-shift for 8-bit format
    .bus_handle = i2c_bus,  // REQUIRED for IDF v5.3+
};
const audio_codec_ctrl_if_t *spk_ctrl = audio_codec_new_i2c_ctrl(&spk_i2c);

es8311_codec_cfg_t es8311_cfg = {
    .ctrl_if = spk_ctrl,
    .gpio_if = audio_codec_new_gpio(),
    .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
    .master_mode = false,
    .use_mclk = true,
    .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    // Other fields = false/NC
};
const audio_codec_if_t *es8311 = es8311_codec_new(&es8311_cfg);

esp_codec_dev_cfg_t spk_cfg = {
    .dev_type = ESP_CODEC_DEV_TYPE_OUT,
    .codec_if = es8311,
    .data_if = data_if,
};
s_play_dev = esp_codec_dev_new(&spk_cfg);
```

### 5. Create microphone codec (ES7243E)
```c
audio_codec_i2c_cfg_t mic_i2c = {
    .port = 0,
    .addr = 0x14 << 1,  // 7-bit addr 0x14, left-shift for 8-bit format
    .bus_handle = i2c_bus,
};
const audio_codec_ctrl_if_t *mic_ctrl = audio_codec_new_i2c_ctrl(&mic_i2c);

es7243e_codec_cfg_t es7243e_cfg = { .ctrl_if = mic_ctrl };
const audio_codec_if_t *es7243e = es7243e_codec_new(&es7243e_cfg);

esp_codec_dev_cfg_t mic_cfg = {
    .dev_type = ESP_CODEC_DEV_TYPE_IN,
    .codec_if = es7243e,
    .data_if = data_if,
};
s_record_dev = esp_codec_dev_new(&mic_cfg);
```

### 6. Open codec devices
```c
esp_codec_dev_sample_info_t fs = {
    .bits_per_sample = 16,
    .channel = 1,
    .sample_rate = 16000,
};

// Speaker: mono
esp_codec_dev_open(s_play_dev, &fs);

// Mic: MUST use channel=2 with channel_mask for stereo extraction
fs.channel = 2;
fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);  // Extract channel 1
esp_codec_dev_open(s_record_dev, &fs);
esp_codec_dev_set_in_gain(s_record_dev, 36.0f);  // Max gain
```

## I2C Address Gotcha

The `audio_codec_i2c_cfg_t.addr` field expects **8-bit (left-shifted)** I2C addresses:
- ES8311: 7-bit=0x18, pass `0x18 << 1 = 0x30`
- ES7243E: 7-bit=0x14, pass `0x14 << 1 = 0x28`
- Internally, the library does `device_address = (addr >> 1)` for the new I2C master API

## I2C Bus Handle (REQUIRED for IDF v5.3+)

When `CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE` is NOT set (our case), the library uses the new `i2c_master.h` API and REQUIRES `bus_handle` to be non-NULL. Without it, `audio_codec_new_i2c_ctrl()` returns `ESP_CODEC_DEV_INVALID_ARG`.

## Reading/Writing Audio

Use `esp_codec_dev_read()` and `esp_codec_dev_write()` with a mutex:
```c
xSemaphoreTake(codec_mutex, portMAX_DELAY);
esp_codec_dev_read(record_dev, buffer, len_bytes);
xSemaphoreGive(codec_mutex);
```

## Volume Control
```c
esp_codec_dev_set_out_vol(play_dev, volume_percent);  // 0-95 (cap at 95 per Seeed)
```

## Key Config Options (sdkconfig)
```
CONFIG_CODEC_ES8311_SUPPORT=y
CONFIG_CODEC_ES7243E_SUPPORT=y
# CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE is not set
```

## What Went Wrong With Raw I2C
Our raw I2C register writes matched the driver's init sequence, but:
1. The `es7243e_adc_enable()` function overwrites PGA gain registers
2. The `esp_codec_dev_open()` call reconfigures the I2S channel for proper stereo extraction
3. Without `channel=2, channel_mask=MASK(1)`, the mono I2S read gets garbage/noise
4. The codec dev framework handles I2S reconfiguration that raw reads miss
