cmake_minimum_required(VERSION 3.22)
project(papaya VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warning flags
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter -pthread)
endif()

# Build options
option(PAPAYA_BUILD_TESTS "Build Papaya test suites" ON)
option(PAPAYA_ENABLE_ASAN "Enable AddressSanitizer for debug builds" OFF)

if (PAPAYA_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# Find system libraries
find_package(Threads REQUIRED)
find_package(Vulkan QUIET)
find_package(PkgConfig QUIET)

if (PKG_CONFIG_FOUND)
    pkg_check_modules(ZSTD libzstd)
    pkg_check_modules(SDL2 sdl2)
endif()

# Include subdirectories
add_subdirectory(src/common)
add_subdirectory(src/hv)
add_subdirectory(src/storage)
add_subdirectory(src/hle)
add_subdirectory(src/gpu)
add_subdirectory(src/audio)
add_subdirectory(src/input)
add_subdirectory(src/frontend)
add_subdirectory(src/app)

if (PAPAYA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
