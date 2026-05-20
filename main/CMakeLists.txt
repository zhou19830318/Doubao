idf_component_register(
    SRCS "app_main.c" "app_state.c" "voice_chat.c" "serial_cmd.c" "app_tasks.c" "mem_monitor.c"
    INCLUDE_DIRS "include"
    REQUIRES board wifi_manager openclaw ui tts stt settings error_log webserver camera wake_word mp3_player notes_manager
    PRIV_REQUIRES console driver esp_event freertos
)
