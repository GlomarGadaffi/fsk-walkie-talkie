# fsk-walkie-talkie — ESP-IDF build (no Arduino). LilyGo T3-S3 + MVSRBoard.
# The PlatformIO/Arduino build in platformio.ini + src/ is the legacy one.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fsk-walkie-talkie)
