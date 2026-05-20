idf_component_register(
    SRCS "webserver.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_http_client json settings error_log openclaw wifi_manager board esp_app_format mdns ui notes_manager
    EMBED_FILES "index.html"
)
