# panel1: the Waveshare ESP32-S3-Touch-LCD-4B, showing the meeting rooms'
# bookings for a day. Its own project, not a board of the screen project.
#   cd panel1 && idf.py build        then   tools/flash-panel1.sh
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(panel1)
