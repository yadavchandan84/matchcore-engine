# The matchcore library. In Phase 1 this holds only version/metadata plumbing;
# later phases add the ITCH parser, order book, matching engine and pipeline.
add_library(matchcore
  matchcore.cpp)

add_library(matchcore::matchcore ALIAS matchcore)

target_include_directories(matchcore
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)

target_link_libraries(matchcore
  PRIVATE
    matchcore_warnings
    matchcore_sanitizers)

target_compile_features(matchcore PUBLIC cxx_std_20)
