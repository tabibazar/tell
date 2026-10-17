# tiny1: a clone of Adafruit's Feather ESP32-S3 TFT. Its own project.
#   cd tiny1 && idf.py build        then   tools/flash-tiny1.sh
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/boardstats")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tiny1)
