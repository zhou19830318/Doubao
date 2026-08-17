idf_component_register(SRCS "doubao_voice.c" "protocol.c" "ws_client.c" "resampler.c"
    INCLUDE_DIRS "include" REQUIRES esp_websocket_client json esp_timer esp_event
    PRIV_REQUIRES mbedtls)  # protocol.c: mbedtls_base64_encode/decode
