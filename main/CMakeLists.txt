idf_component_register(
    SRCS "proto_test.c" "app_main.c" "app_state.c" "app_state_machine.c" "serial_cmd.c" "app_tasks.c" "mem_monitor.c" "doubao_chat.c"
    INCLUDE_DIRS "include"
    REQUIRES board wifi_manager ui settings error_log webserver wake_word mp3_player notes_manager doubao_voice
    PRIV_REQUIRES console driver esp_event freertos esp_lvgl_port
)
