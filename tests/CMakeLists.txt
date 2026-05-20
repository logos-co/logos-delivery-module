cmake_minimum_required(VERSION 3.14)
project(DeliveryModuleTests LANGUAGES CXX)

include(LogosTest)

# Unit tests (mocked liblogosdelivery)

logos_test(
    NAME delivery_module_tests
    MODULE_SOURCES
        ../src/delivery_module_plugin.cpp
        mocks/delivery_module_events_stub.cpp
    TEST_SOURCES
        main.cpp
        test_delivery_module.cpp
    MOCK_C_SOURCES
        mocks/mock_liblogosdelivery.cpp
    EXTRA_INCLUDES
        stubs/lib
)

# delivery_module uses C++20 features (std::binary_semaphore, pack init-capture).
# LogosTest.cmake defaults to C++17; override per-target.
set_target_properties(delivery_module_tests PROPERTIES CXX_STANDARD 20)

# Integration tests (real liblogosdelivery)

find_library(LIBLOGOSDELIVERY_PATH
    NAMES liblogosdelivery.so liblogosdelivery.dylib
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../lib
    NO_DEFAULT_PATH)

if(LIBLOGOSDELIVERY_PATH)
    message(STATUS "[DeliveryTests] liblogosdelivery found: ${LIBLOGOSDELIVERY_PATH} - building integration tests")

    logos_test(
        NAME delivery_module_integration_tests
        MODULE_SOURCES
            ../src/delivery_module_plugin.cpp
            mocks/delivery_module_events_stub.cpp
        TEST_SOURCES
            main.cpp
            test_delivery_integration.cpp
        EXTRA_INCLUDES
            ../lib
        EXTRA_LINK_LIBS
            ${LIBLOGOSDELIVERY_PATH}
    )

    set_target_properties(delivery_module_integration_tests PROPERTIES CXX_STANDARD 20)

    # TODO: logos-module-builder mkLogosModuleTests.nix (line ~100) joins RPATH
    # entries with ":" but CMake expects ";". Fix upstream in mkLogosModuleTests.nix,
    # then remove this block.
    if(APPLE)
        # On macOS, liblogosdelivery.dylib has a Cargo-baked absolute path to
        # librln.dylib. The preConfigure hook rewrites it to @rpath/librln.dylib
        # in the local lib/ copy. Keep the local lib/ dir in RPATH so the patched
        # copy is loaded — the unpatched Nix store copy would fail to find librln.
        # The macOS sandbox dir is not /build/ so this RPATH is not forbidden.
        get_filename_component(LIBLOGOSDELIVERY_DIR "${LIBLOGOSDELIVERY_PATH}" DIRECTORY)
        set_target_properties(delivery_module_integration_tests PROPERTIES
            INSTALL_RPATH "${LIBLOGOSDELIVERY_DIR}"
            BUILD_WITH_INSTALL_RPATH ON
        )
    else()
        # On Linux, the build dir is /build/ which is a forbidden reference in
        # Nix output. Use the Nix store paths from CMAKE_INSTALL_RPATH instead,
        # replacing the colon separator with the semicolon CMake expects.
        string(REPLACE ":" ";" FIXED_RPATH "${CMAKE_INSTALL_RPATH}")
        set_target_properties(delivery_module_integration_tests PROPERTIES
            INSTALL_RPATH "${FIXED_RPATH}"
            BUILD_WITH_INSTALL_RPATH ON
        )
    endif()
else()
    message(STATUS "[DeliveryTests] liblogosdelivery not found in ../lib - skipping integration tests")
endif()
