cmake_minimum_required(VERSION 3.14)
project(grovedb-cpp VERSION 1.0.0 LANGUAGES CXX)

# Options
option(GROVEDB_CPP_BUILD_TESTS "Build tests" OFF)
option(GROVEDB_CPP_BUILD_EXAMPLES "Build examples" OFF)

# Find grovedb_ffi library
# Users can set GROVEDB_FFI_DIR to the directory containing libgrovedb_ffi
if(DEFINED GROVEDB_FFI_DIR)
    find_library(GROVEDB_FFI_LIBRARY
        NAMES grovedb_ffi
        PATHS "${GROVEDB_FFI_DIR}" "${GROVEDB_FFI_DIR}/lib"
        NO_DEFAULT_PATH
    )
    find_path(GROVEDB_FFI_INCLUDE_DIR
        NAMES grovedb.h
        PATHS "${GROVEDB_FFI_DIR}" "${GROVEDB_FFI_DIR}/include"
        NO_DEFAULT_PATH
    )
else()
    # Try pkg-config
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GROVEDB_FFI QUIET grovedb_ffi)
        if(GROVEDB_FFI_FOUND)
            set(GROVEDB_FFI_LIBRARY ${GROVEDB_FFI_LIBRARIES})
            set(GROVEDB_FFI_INCLUDE_DIR ${GROVEDB_FFI_INCLUDE_DIRS})
        endif()
    endif()

    # Fallback: search standard paths
    if(NOT GROVEDB_FFI_LIBRARY)
        find_library(GROVEDB_FFI_LIBRARY NAMES grovedb_ffi)
    endif()
    if(NOT GROVEDB_FFI_INCLUDE_DIR)
        find_path(GROVEDB_FFI_INCLUDE_DIR NAMES grovedb.h)
    endif()
endif()

if(NOT GROVEDB_FFI_LIBRARY)
    message(WARNING "grovedb_ffi library not found. Set GROVEDB_FFI_DIR or ensure it's in the library path.")
endif()

if(NOT GROVEDB_FFI_INCLUDE_DIR)
    message(WARNING "grovedb.h not found. Set GROVEDB_FFI_DIR or ensure it's in the include path.")
endif()

# Create header-only (INTERFACE) library
add_library(grovedb-cpp INTERFACE)
add_library(grovedb::grovedb-cpp ALIAS grovedb-cpp)

target_include_directories(grovedb-cpp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(GROVEDB_FFI_INCLUDE_DIR)
    target_include_directories(grovedb-cpp INTERFACE
        $<BUILD_INTERFACE:${GROVEDB_FFI_INCLUDE_DIR}>
    )
endif()

if(GROVEDB_FFI_LIBRARY)
    target_link_libraries(grovedb-cpp INTERFACE ${GROVEDB_FFI_LIBRARY})
endif()

target_compile_features(grovedb-cpp INTERFACE cxx_std_17)

# Installation
include(GNUInstallDirs)

install(TARGETS grovedb-cpp
    EXPORT grovedb-cpp-targets
)

install(DIRECTORY include/grovedb
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT grovedb-cpp-targets
    FILE grovedb-cpp-targets.cmake
    NAMESPACE grovedb::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/grovedb-cpp
)

# Generate package config
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/grovedb-cpp-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/grovedb-cpp-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/grovedb-cpp
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/grovedb-cpp-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/grovedb-cpp-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/grovedb-cpp-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/grovedb-cpp
)

# Tests
if(GROVEDB_CPP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Examples
if(GROVEDB_CPP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
