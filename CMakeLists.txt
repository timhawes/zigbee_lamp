cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")

# -DLAMP_PROFILE=production selects the lamp hardware (touch pad, PWM to the LED driver). Use a separate
# build directory and sdkconfig for it (see README "Build profiles").
if(LAMP_PROFILE STREQUAL "production")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.production")
elseif(LAMP_PROFILE)
    message(FATAL_ERROR "Unknown LAMP_PROFILE '${LAMP_PROFILE}' (expected: production)")
endif()

# Local settings that must not be committed (the Hue link key) live in sdkconfig.local.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.local")
else()
    message(WARNING "sdkconfig.local not found: no Hue link key, the lamp won't pair with a Hue bridge")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lamp)
