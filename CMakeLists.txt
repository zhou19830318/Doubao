# AIWearable - ESP32 OpenClaw Interface Device
# Requires ESP-IDF v5.5+

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aiwearable)
