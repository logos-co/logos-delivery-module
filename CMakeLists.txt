cmake_minimum_required(VERSION 3.14)
project(LogosDeliveryModulePlugin LANGUAGES CXX)

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

# Define the module
logos_module(
    NAME delivery_module
    SOURCES
        src/delivery_module_plugin.h
        src/delivery_module_plugin.cpp
    EXTERNAL_LIBS
        logosdelivery
    INCLUDE_DIRS
        lib
)
