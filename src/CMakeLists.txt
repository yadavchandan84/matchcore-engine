# The matchcore library: version metadata and the ITCH 5.0 parser. Later phases
# add the order book, matching engine and pipeline.
add_library(matchcore
  matchcore.cpp
  itch/parser.cpp
  itch/synthetic.cpp
  book/reference_book.cpp
  book/order_book.cpp)

add_library(matchcore::matchcore ALIAS matchcore)

target_include_directories(matchcore
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)

target_link_libraries(matchcore
  PRIVATE
    matchcore_warnings
    matchcore_sanitizers)

target_compile_features(matchcore PUBLIC cxx_std_20)
