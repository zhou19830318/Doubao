idf_component_register(
    SRCS "stt_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp_websocket_client json esp_psram esp_timer mbedtls esp_ringbuf
)