cmake_minimum_required(VERSION 3.22)
project(papaya VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler warning flags
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter -pthread -O3)
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

# Include subdirectories
add_subdirectory(src/common)
add_subdirectory(src/steam)
add_subdirectory(src/cpu)
add_subdirectory(src/gpu)
add_subdirectory(src/profile)
add_subdirectory(src/kernel)
add_subdirectory(src/audio)
add_subdirectory(src/input)
add_subdirectory(src/frontend)
add_subdirectory(src/app)

if (PAPAYA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
