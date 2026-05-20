set(camera_requires "board")
set(camera_priv "")

if(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND camera_requires "sscma_client")
    list(APPEND camera_priv "driver")
endif()

idf_component_register(
    SRCS "camera.c"
    INCLUDE_DIRS "include"
    REQUIRES ${camera_requires}
    PRIV_REQUIRES ${camera_priv}
)

