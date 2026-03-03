# examples.cmake

# Example executable
set(EXAMPLE_SOURCES
    examples/simple.cpp
)

add_executable(simple_example ${EXAMPLE_SOURCES})

# Include directories (reuse same paths as plugin)
target_include_directories(simple_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}  # root
)

# Include liblogos headers
if(_liblogos_is_source)
    target_include_directories(simple_example PRIVATE ${LOGOS_LIBLOGOS_ROOT})
else()
    target_include_directories(simple_example PRIVATE ${LOGOS_LIBLOGOS_ROOT}/include)
endif()

# Include SDK headers
if(_cpp_sdk_is_source)
    target_include_directories(simple_example PRIVATE 
        ${LOGOS_CPP_SDK_ROOT}/cpp
        ${LOGOS_CPP_SDK_ROOT}/cpp/generated
    )
else()
    target_include_directories(simple_example PRIVATE 
        ${LOGOS_CPP_SDK_ROOT}/include
        ${LOGOS_CPP_SDK_ROOT}/include/cpp
        ${LOGOS_CPP_SDK_ROOT}/include/core
    )
endif()

# Link Qt libraries
target_link_libraries(simple_example PRIVATE 
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
)

# Link SDK library if installed layout
if(NOT _cpp_sdk_is_source)
    target_link_libraries(simple_example PRIVATE ${LOGOS_SDK_LIB})
endif()

# Link liblogosdelivery
if(LIBLOGOSDELIVERY_PATH)
    target_link_libraries(simple_example PRIVATE ${LIBLOGOSDELIVERY_PATH})
endif()

# Ensure C++20 standard
target_compile_features(simple_example PRIVATE cxx_std_20)

# Ensure generator runs first
add_dependencies(simple_example run_cpp_generator_messaging)

# Optional: output directory same as plugin
set_target_properties(simple_example PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/modules"
)