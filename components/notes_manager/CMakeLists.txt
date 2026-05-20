idf_component_register(
    SRCS "notes_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs
    PRIV_REQUIRES json
)
