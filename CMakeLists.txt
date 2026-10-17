cmake_minimum_required(VERSION 3.16)

# Local settings that must not be committed (the Hue link key) live in sdkconfig.local.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.local")
else()
    message(WARNING "sdkconfig.local not found: no Hue link key, the lamp won't pair with a Hue bridge")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lamp)
