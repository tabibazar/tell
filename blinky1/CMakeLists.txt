# blinky1: a generic ESP32-S3-DevKitC-1 N16R8 clone. Its own project: one RGB
# LED that watch switches over ESP-NOW (main/lightlink.h, shared with watch).
#   cd blinky1 && idf.py build        then   tools/flash-blinky1.sh
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(blinky1)
