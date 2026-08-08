cmake_minimum_required(VERSION 3.5)

# Pull in the ESP32-S31-Korvo board support package that ships with esp-dev-kits.
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/../ESPressif/esp-dev-kits/examples/esp32-s31-korvo/examples/common_components
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(therminesp)
