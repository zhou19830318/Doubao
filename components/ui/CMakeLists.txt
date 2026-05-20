idf_component_register(
    SRCS
        # Core UI modules
        "src/ui_main.c"
        "src/ui_status_bar.c"
        "src/ui_state_chip.c"
        "src/ui_mp3_ui.c"
        "src/ui_provisioning.c"
        # GIF state manager
        "ui_state_gif.c"
        # Feature modules
        "ui_tasks.c"
        "ui_rgb_led.c"
        # Fonts
        "fonts/SourceHanSansCN_Medium_16.c"
    INCLUDE_DIRS "include" "src" "fonts"
    REQUIRES board openclaw lvgl mp3_player
    PRIV_REQUIRES esp_timer esp_lvgl_port vfs
)
