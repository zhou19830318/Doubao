idf_component_register(
    SRCS
        # Core UI modules
        "src/ui_main.c"
        "src/ui_status_bar.c"
        "src/ui_state_chip.c"
        "src/ui_mp3_ui.c"
        "src/ui_provisioning.c"
        # Feature modules
        "ui_tasks.c"
        "ui_rgb_led.c"
        # Fonts
        "fonts/SourceHanSansCN_Medium_16.c"
        "fonts/ui_font_ClockNum.c"
        "fonts/ui_font_DateNum.c"
    INCLUDE_DIRS "include" "src" "fonts"
    REQUIRES board lvgl mp3_player
    PRIV_REQUIRES esp_timer esp_lvgl_port vfs
)
