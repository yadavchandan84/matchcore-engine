include(FetchContent)

# Pin GoogleTest to a known release for reproducible builds.
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2)

# Honor the parent project's compiler/standard settings.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Treat GoogleTest headers as system headers so our strict warning set does not
# fire on third-party code included into our test translation units.
foreach(gtest_target gtest gtest_main gmock)
  if(TARGET ${gtest_target})
    get_target_property(gtest_inc ${gtest_target} INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(${gtest_target} PROPERTIES
      INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${gtest_inc}")
  endif()
endforeach()

add_executable(matchcore_tests
  smoke_test.cpp
  types_test.cpp
  itch_parser_test.cpp
  book_test.cpp
  matching_engine_test.cpp
  pipeline_test.cpp)

find_package(Threads REQUIRED)

target_link_libraries(matchcore_tests
  PRIVATE
    matchcore::matchcore
    matchcore_warnings
    matchcore_sanitizers
    Threads::Threads
    GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(matchcore_tests DISCOVERY_TIMEOUT 60)

# Zero-allocation proof. Lives in its own executable because it replaces the
# global operator new/delete to count allocations, which must not interfere
# with GoogleTest or the other tests.
#
# Skipped under sanitizers: they install their own allocator, which conflicts
# with replacing operator new, and the count would not be meaningful anyway.
if(NOT MATCHCORE_ENABLE_ASAN AND NOT MATCHCORE_ENABLE_TSAN AND NOT MATCHCORE_ENABLE_UBSAN)
  add_executable(alloc_test alloc_test.cpp)
  target_link_libraries(alloc_test
    PRIVATE
      matchcore::matchcore
      matchcore_warnings)
  add_test(NAME ZeroAllocation.HotPath COMMAND alloc_test)
endif()
