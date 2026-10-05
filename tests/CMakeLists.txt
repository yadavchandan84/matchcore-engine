include(FetchContent)

# Pin GoogleTest to a known release for reproducible builds.
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2)

# Honor the parent project's compiler/standard settings.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(matchcore_tests
  smoke_test.cpp
  types_test.cpp)

target_link_libraries(matchcore_tests
  PRIVATE
    matchcore::matchcore
    matchcore_warnings
    matchcore_sanitizers
    GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(matchcore_tests)
