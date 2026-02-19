cmake_minimum_required(VERSION 3.14)
project(DeliveryModulePlugin LANGUAGES CXX)

# Require C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

set(CMAKE_AUTOMOC ON)

option(LOGOS_MESSAGING_MODULE_USE_VENDOR "Force use of vendored Logos dependencies" OFF)

# Allow override from environment or command line
if(NOT DEFINED LOGOS_LIBLOGOS_ROOT)
    set(_parent_liblogos "${CMAKE_CURRENT_SOURCE_DIR}/../logos-liblogos")
    set(_use_vendor ${LOGOS_MESSAGING_MODULE_USE_VENDOR})
    if(NOT _use_vendor)
        if(NOT EXISTS "${_parent_liblogos}/interface.h")
            set(_use_vendor ON)
        endif()
    endif()
    if(_use_vendor)
        set(LOGOS_LIBLOGOS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/logos-liblogos")
    else()
        set(LOGOS_LIBLOGOS_ROOT "${_parent_liblogos}")
    endif()
endif()

if(NOT DEFINED LOGOS_CPP_SDK_ROOT)
    set(_parent_cpp_sdk "${CMAKE_CURRENT_SOURCE_DIR}/../logos-cpp-sdk")
    set(_use_vendor ${LOGOS_MESSAGING_MODULE_USE_VENDOR})
    if(NOT _use_vendor)
        if(NOT EXISTS "${_parent_cpp_sdk}/cpp/logos_api.h")
            set(_use_vendor ON)
        endif()
    endif()
    if(_use_vendor)
        set(LOGOS_CPP_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/logos-cpp-sdk")
    else()
        set(LOGOS_CPP_SDK_ROOT "${_parent_cpp_sdk}")
    endif()
endif()

if(NOT DEFINED LOGOS_DELIVERY_ROOT)
    set(LOGOS_DELIVERY_ROOT "$ENV{LOGOS_DELIVERY_ROOT}")
endif()

# Check if dependencies are available (support both source and installed layouts)
set(_liblogos_found FALSE)
if(EXISTS "${LOGOS_LIBLOGOS_ROOT}/interface.h")
    set(_liblogos_found TRUE)
    set(_liblogos_is_source TRUE)
elseif(EXISTS "${LOGOS_LIBLOGOS_ROOT}/include/interface.h")
    set(_liblogos_found TRUE)
    set(_liblogos_is_source FALSE)
endif()

set(_cpp_sdk_found FALSE)
if(EXISTS "${LOGOS_CPP_SDK_ROOT}/cpp/logos_api.h")
    set(_cpp_sdk_found TRUE)
    set(_cpp_sdk_is_source TRUE)
elseif(EXISTS "${LOGOS_CPP_SDK_ROOT}/include/cpp/logos_api.h")
    set(_cpp_sdk_found TRUE)
    set(_cpp_sdk_is_source FALSE)
endif()

if(NOT _liblogos_found)
    message(FATAL_ERROR "logos-liblogos not found at ${LOGOS_LIBLOGOS_ROOT}. "
                        "Set LOGOS_LIBLOGOS_ROOT or run git submodule update --init --recursive.")
endif()

if(NOT _cpp_sdk_found)
    message(FATAL_ERROR "logos-cpp-sdk not found at ${LOGOS_CPP_SDK_ROOT}. "
                        "Set LOGOS_CPP_SDK_ROOT or run git submodule update --init --recursive.")
endif()

# Root that contains the vendored dependencies (logos-core checkout or script vendor directory)
get_filename_component(LOGOS_DEPS_ROOT "${LOGOS_CPP_SDK_ROOT}" DIRECTORY)

# Find Qt RemoteObjects (needed for LogosAPI)
if(NOT DEFINED QT_VERSION_MAJOR)
    find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core RemoteObjects)
    if(Qt6_FOUND)
        set(QT_VERSION_MAJOR 6)
    else()
        set(QT_VERSION_MAJOR 5)
    endif()
endif()
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core RemoteObjects)

# Run Logos C++ generator on metadata before compilation
set(METADATA_JSON "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json")
set(PLUGINS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/modules")

# Try to find the generator binary
if(_cpp_sdk_is_source)
    # Source layout: build the generator
    set(CPP_GENERATOR_BUILD_DIR "${LOGOS_DEPS_ROOT}/build/cpp-generator")
    set(CPP_GENERATOR "${CPP_GENERATOR_BUILD_DIR}/bin/logos-cpp-generator")
    
    if(NOT TARGET cpp_generator_build)
        add_custom_target(cpp_generator_build
            COMMAND bash "${LOGOS_CPP_SDK_ROOT}/cpp-generator/compile.sh"
            WORKING_DIRECTORY "${LOGOS_DEPS_ROOT}"
            COMMENT "Building logos-cpp-generator via ${LOGOS_CPP_SDK_ROOT}/cpp-generator/compile.sh"
            VERBATIM
        )
    endif()
    
    add_custom_target(run_cpp_generator_messaging
        COMMAND "${CPP_GENERATOR}" --metadata "${METADATA_JSON}" --module-dir "${PLUGINS_OUTPUT_DIR}"
        WORKING_DIRECTORY "${LOGOS_DEPS_ROOT}"
        COMMENT "Running logos-cpp-generator on ${METADATA_JSON} with module-dir ${PLUGINS_OUTPUT_DIR}"
        VERBATIM
    )
    add_dependencies(run_cpp_generator_messaging cpp_generator_build)
else()
    # Installed layout: use the generator from the install path
    find_program(CPP_GENERATOR logos-cpp-generator PATHS ${LOGOS_CPP_SDK_ROOT}/bin NO_DEFAULT_PATH REQUIRED)
    
    add_custom_target(run_cpp_generator_messaging
        COMMAND "${CPP_GENERATOR}" --metadata "${METADATA_JSON}" --module-dir "${PLUGINS_OUTPUT_DIR}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT "Running logos-cpp-generator on ${METADATA_JSON} with module-dir ${PLUGINS_OUTPUT_DIR}"
        VERBATIM
    )
endif()

# Find liblogosdelivery library
set(LIBLOGOSDELIVERY_DIR "${LOGOS_DELIVERY_ROOT}/bin")
# Prioritize platform-specific library names
if(APPLE)
    set(LIBLOGOSDELIVERY_NAMES liblogosdelivery.dylib liblogosdelivery.so)
elseif(WIN32)
    set(LIBLOGOSDELIVERY_NAMES liblogosdelivery.dll lmapi.dll)
else()
    set(LIBLOGOSDELIVERY_NAMES liblogosdelivery.so)
endif()
find_library(LIBLOGOSDELIVERY_PATH NAMES ${LIBLOGOSDELIVERY_NAMES} PATHS ${LIBLOGOSDELIVERY_DIR} NO_DEFAULT_PATH)

# Plugin sources
set(PLUGIN_SOURCES
    delivery_module_plugin.cpp
    delivery_module_plugin.h
    delivery_module_interface.h
)

# Add liblogos interface header
if(_liblogos_is_source)
    list(APPEND PLUGIN_SOURCES ${LOGOS_LIBLOGOS_ROOT}/interface.h)
else()
    list(APPEND PLUGIN_SOURCES ${LOGOS_LIBLOGOS_ROOT}/include/interface.h)
endif()

# Add SDK sources (only if source layout, installed layout uses the library)
if(_cpp_sdk_is_source)
    list(APPEND PLUGIN_SOURCES
        ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api.cpp
        ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api.h
        ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api_client.cpp
        ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api_client.h
        ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api_consumer.cpp
        ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api_consumer.h
        ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api_provider.cpp
        ${LOGOS_CPP_SDK_ROOT}/cpp/logos_api_provider.h
        ${LOGOS_CPP_SDK_ROOT}/cpp/token_manager.cpp
        ${LOGOS_CPP_SDK_ROOT}/cpp/token_manager.h
        ${LOGOS_CPP_SDK_ROOT}/cpp/module_proxy.cpp
        ${LOGOS_CPP_SDK_ROOT}/cpp/module_proxy.h
    )
endif()

# Create the plugin library
add_library(delivery_module_plugin SHARED ${PLUGIN_SOURCES})

# Set output name without lib prefix
set_target_properties(delivery_module_plugin PROPERTIES
    PREFIX "")

# Ensure generator runs before building the plugin
add_dependencies(delivery_module_plugin run_cpp_generator_messaging)

# Link Qt libraries
target_link_libraries(delivery_module_plugin PRIVATE 
    Qt${QT_VERSION_MAJOR}::Core 
    Qt${QT_VERSION_MAJOR}::RemoteObjects
)

# Link SDK library if using installed layout
if(NOT _cpp_sdk_is_source)
    find_library(LOGOS_SDK_LIB logos_sdk PATHS ${LOGOS_CPP_SDK_ROOT}/lib NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(delivery_module_plugin PRIVATE ${LOGOS_SDK_LIB})
endif()

# Link to liblogosdelivery on all platforms
if(LIBLOGOSDELIVERY_PATH)
    target_link_libraries(delivery_module_plugin PRIVATE ${LIBLOGOSDELIVERY_PATH})
else()
    message(WARNING "liblogosdelivery not found in ${LIBLOGOSDELIVERY_DIR}. Build or provide it before linking.")
endif()

# Include directories
target_include_directories(delivery_module_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBLOGOSDELIVERY_DIR}
)

# Look for liblogosdelivery.h in logos-delivery
if(EXISTS "${LOGOS_DELIVERY_ROOT}/include/liblogosdelivery.h")
    target_include_directories(delivery_module_plugin PRIVATE ${LOGOS_DELIVERY_ROOT}/include)
elseif(EXISTS "${LOGOS_DELIVERY_ROOT}/liblogosdelivery/liblogosdelivery.h")
    target_include_directories(delivery_module_plugin PRIVATE ${LOGOS_DELIVERY_ROOT}/liblogosdelivery)
endif()

# Add include directories based on layout type
if(_liblogos_is_source)
    target_include_directories(delivery_module_plugin PRIVATE ${LOGOS_LIBLOGOS_ROOT})
else()
    target_include_directories(delivery_module_plugin PRIVATE ${LOGOS_LIBLOGOS_ROOT}/include)
endif()

if(_cpp_sdk_is_source)
    target_include_directories(delivery_module_plugin PRIVATE 
        ${LOGOS_CPP_SDK_ROOT}/cpp
        ${LOGOS_CPP_SDK_ROOT}/cpp/generated
    )
else()
    target_include_directories(delivery_module_plugin PRIVATE 
        ${LOGOS_CPP_SDK_ROOT}/include
        ${LOGOS_CPP_SDK_ROOT}/include/cpp
        ${LOGOS_CPP_SDK_ROOT}/include/core
    )
endif()

set_target_properties(delivery_module_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules"  # For Windows .dll
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH FALSE)

if(APPLE)
    # Allow unresolved symbols at link time; liblogosdelivery will be provided at runtime
    target_link_options(delivery_module_plugin PRIVATE -undefined dynamic_lookup)
    set_target_properties(delivery_module_plugin PROPERTIES
        INSTALL_RPATH "@loader_path"
        INSTALL_NAME_DIR "@rpath"
        BUILD_WITH_INSTALL_NAME_DIR TRUE)

    if(LIBLOGOSDELIVERY_PATH)
        get_filename_component(LIBLOGOSDELIVERY_FILENAME "${LIBLOGOSDELIVERY_PATH}" NAME)
        add_custom_command(TARGET delivery_module_plugin PRE_LINK
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${LIBLOGOSDELIVERY_PATH}
                ${CMAKE_BINARY_DIR}/modules/${LIBLOGOSDELIVERY_FILENAME}
            COMMENT "Copying ${LIBLOGOSDELIVERY_FILENAME} to modules directory"
        )

        add_custom_command(TARGET delivery_module_plugin POST_BUILD
            COMMAND install_name_tool -id "@rpath/delivery_module_plugin.dylib" $<TARGET_FILE:delivery_module_plugin>
            COMMAND install_name_tool -id "@rpath/${LIBLOGOSDELIVERY_FILENAME}" "${CMAKE_BINARY_DIR}/modules/${LIBLOGOSDELIVERY_FILENAME}"
            COMMAND install_name_tool -change "${LIBLOGOSDELIVERY_PATH}" "@rpath/${LIBLOGOSDELIVERY_FILENAME}" $<TARGET_FILE:delivery_module_plugin>
            COMMENT "Updating library paths for macOS"
        )
    else()
        add_custom_command(TARGET delivery_module_plugin POST_BUILD
            COMMAND install_name_tool -id "@rpath/delivery_module_plugin.dylib" $<TARGET_FILE:delivery_module_plugin>
            COMMENT "Updating library paths for macOS (liblogosdelivery not found)"
        )
    endif()
else()
    set_target_properties(delivery_module_plugin PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        INSTALL_RPATH_USE_LINK_PATH FALSE)

    if(LIBLOGOSDELIVERY_PATH)
        get_filename_component(LIBLOGOSDELIVERY_FILENAME "${LIBLOGOSDELIVERY_PATH}" NAME)
        add_custom_command(TARGET delivery_module_plugin PRE_LINK
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${LIBLOGOSDELIVERY_PATH}
                ${CMAKE_BINARY_DIR}/modules/${LIBLOGOSDELIVERY_FILENAME}
            COMMENT "Copying ${LIBLOGOSDELIVERY_FILENAME} to modules directory"
        )
    endif()
endif()

install(TARGETS delivery_module_plugin
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/logos/modules
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/logos/modules
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/logos/modules
)

install(FILES ${METADATA_JSON}
    DESTINATION ${CMAKE_INSTALL_DATADIR}/logos-delivery-module
)

install(DIRECTORY "${PLUGINS_OUTPUT_DIR}/"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/logos-delivery-module/generated
    OPTIONAL
)
