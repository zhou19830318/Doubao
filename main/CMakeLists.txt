idf_component_register(
    SRCS "vad.c" "app_main.c" "app_state.c" "app_state_machine.c" "app_tasks.c" "doubao_chat.c"
    INCLUDE_DIRS "include"
    REQUIRES board wifi_manager ui settings error_log wake_word doubao_voice
    PRIV_REQUIRES console driver esp_event freertos esp_lvgl_port
)
