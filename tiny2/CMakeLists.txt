# tiny2: a generic ESP32-S3-DevKitC-1 N16R8 clone. Its own project: one RGB
#   cd tiny2 && idf.py build        then   tools/flash-tiny2.sh
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tiny2)
