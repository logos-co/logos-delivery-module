cmake_minimum_required(VERSION 3.14)
project(LogosDeliveryModulePlugin LANGUAGES C CXX)

# Require C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include the Logos Module CMake helper
if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

# The generated liblogosdelivery binding encodes every call with TinyCBOR;
# flake.nix stages nim-ffi's vendored copy into lib/tinycbor.
file(GLOB TINYCBOR_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/lib/tinycbor/*.c)

# Define the module
logos_module(
    NAME delivery_module
    SOURCES
        src/delivery_module_plugin.h
        src/delivery_module_plugin.cpp
        src/rln_bridge.h
        src/rln_bridge.cpp
        src/rln_presets.h
        src/rln_presets.cpp
        ${TINYCBOR_SOURCES}
    EXTERNAL_LIBS
        logosdelivery
    INCLUDE_DIRS
        lib
)
