idf_component_register(
    SRCS "tts_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client board mbedtls json esp_psram
)
