set(BOARD_REQUIRES
    driver
    esp_adc
    esp_lcd
    led_strip
    esp_timer
    lvgl
    esp_lvgl_port
    fatfs
    sdmmc
    esp_driver_sdmmc
    esp_driver_sdspi
)

set(BOARD_PRIV_REQUIRES
    esp_driver_i2s
    esp_driver_i2c
    esp_driver_spi
    esp_driver_gpio
    esp_driver_ledc
    esp_driver_uart
)

# Board-specific source files
set(BOARD_SRCS "board_loopback_test.c" "board.c")

# Waveshare Audio board: compile JD9853 LCD driver + AXS5106L touch driver
if(CONFIG_AIDB_BOARD_WAVESHARE_AUDIO)
    list(APPEND BOARD_SRCS "touch_axs5106l.c")
    list(APPEND BOARD_PRIV_REQUIRES
        esp_io_expander
        esp_io_expander_tca95xx_16bit
    )
endif()

# ESP32-S3 boards need IO expander + codec (for Waveshare Audio)
if(IDF_TARGET STREQUAL "esp32s3")
    # Common ESP32-S3 requirements
    list(APPEND BOARD_REQUIRES
        esp_codec_dev
    )
endif()

idf_component_register(
    SRCS ${BOARD_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${BOARD_REQUIRES}
    PRIV_REQUIRES ${BOARD_PRIV_REQUIRES}
)
