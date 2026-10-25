include(FetchContent)

# Pin Google Benchmark to a known release for reproducible builds.
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.1)
FetchContent_MakeAvailable(googlebenchmark)

find_package(Threads REQUIRED)

add_executable(matchcore_bench microbench.cpp)
target_link_libraries(matchcore_bench
  PRIVATE
    matchcore::matchcore
    matchcore_warnings
    Threads::Threads
    benchmark::benchmark)
target_include_directories(matchcore_bench SYSTEM PRIVATE
  $<TARGET_PROPERTY:benchmark::benchmark,INTERFACE_INCLUDE_DIRECTORIES>)
