# stan-claw: a tap-to-talk assistant that is also a local MCP server for its
# speaker, mics and screen. For the Waveshare ESP32-S3-Touch-LCD-4B.
#   cd stan-claw && idf.py build        then   tools/flash-stan-claw.sh
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(stan-claw)
