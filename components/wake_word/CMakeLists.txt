idf_component_register(
    SRCS "wake_word.c"
    INCLUDE_DIRS "include"
    REQUIRES board freertos esp_common
)
