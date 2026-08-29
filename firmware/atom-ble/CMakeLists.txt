# AtomS3 Lite CSI + BLE iBeacon node
# Slim fork of ruvnet/RuView firmware/esp32-csi-node v0.8.4 (c7b3f0bcc).
# Requires ESP-IDF v5.4.x. Target: ESP32-S3.
cmake_minimum_required(VERSION 3.16)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" PROJECT_VER LIMIT_COUNT 1)
string(STRIP "${PROJECT_VER}" PROJECT_VER)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-csi-node VERSION ${PROJECT_VER})
