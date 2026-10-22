# 智能药盒固件（ESP32-S3 / ESP-IDF 5.x）
# 主机单元测试不走这个 CMake，见 test/host/build_run.bat
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smart_pillbox)
