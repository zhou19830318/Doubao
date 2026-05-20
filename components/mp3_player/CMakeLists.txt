idf_component_register(
    SRCS "mp3_player.c"
    INCLUDE_DIRS "include"
    REQUIRES board wake_word esp_audio_simple_player esp_wifi
)
