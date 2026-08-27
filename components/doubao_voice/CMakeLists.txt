idf_component_register(SRCS "doubao_voice.c" "protocol.c" "ws_client.c" "resampler.c" "audio_in.c" "audio_out.c" "doubao_test.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_websocket_client json esp_timer esp_event board
    PRIV_REQUIRES mbedtls esp_wifi)
