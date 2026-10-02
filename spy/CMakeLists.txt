# spy: the Waveshare ESP32-S3-SIM7670G-4G. Its own project, not a board of the
# screen project at the top: no display, a 4G modem and a camera instead.
#   cd spy && idf.py build        then   tools/flash-spy.sh
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(spy)
