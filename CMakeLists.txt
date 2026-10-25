cmake_minimum_required(VERSION 3.24)

project(matchcore
  VERSION 1.0.0
  DESCRIPTION "MatchCore: Low-Latency Limit Order Book & Matching Engine"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

# Export compile_commands.json for clang-tidy and editors.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Place all binaries in a predictable location.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Treat MatchCore's own code to a strict warning set, without forcing those
# flags onto third-party dependencies.
add_library(matchcore_warnings INTERFACE)
target_compile_options(matchcore_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(MATCHCORE_BUILD_TESTS "Build unit tests" ON)
option(MATCHCORE_BUILD_TOOLS "Build command-line tools (itch_inspect, ...)" ON)
option(MATCHCORE_BUILD_BENCHMARKS "Build Google Benchmark microbenchmarks" OFF)
option(MATCHCORE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MATCHCORE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(MATCHCORE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

add_library(matchcore_sanitizers INTERFACE)
if(MATCHCORE_ENABLE_ASAN)
  target_compile_options(matchcore_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(matchcore_sanitizers INTERFACE -fsanitize=address)
endif()
if(MATCHCORE_ENABLE_UBSAN)
  target_compile_options(matchcore_sanitizers INTERFACE -fsanitize=undefined -fno-omit-frame-pointer)
  target_link_options(matchcore_sanitizers INTERFACE -fsanitize=undefined)
endif()
if(MATCHCORE_ENABLE_TSAN)
  target_compile_options(matchcore_sanitizers INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(matchcore_sanitizers INTERFACE -fsanitize=thread)
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
# Testing must be enabled before any subdirectory registers tests (tools/
# adds an end-to-end test of the CLI tools).
if(MATCHCORE_BUILD_TESTS)
  include(CTest)
  enable_testing()
endif()

add_subdirectory(src)

if(MATCHCORE_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(MATCHCORE_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

if(MATCHCORE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
