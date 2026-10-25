# Command-line tools built on the matchcore library.

foreach(tool itch_inspect itch_gen replay_bench matchcore_demo)
  add_executable(${tool} ${tool}.cpp)
  target_link_libraries(${tool}
    PRIVATE
      matchcore::matchcore
      matchcore_warnings
      matchcore_sanitizers)
endforeach()

# End-to-end smoke test of the tools: generate a session file, then inspect it.
# itch_inspect exits non-zero on any unknown/malformed/trailing bytes.
if(MATCHCORE_BUILD_TESTS)
  set(fixture "${CMAKE_BINARY_DIR}/fixture.NASDAQ_ITCH50")
  add_test(NAME Tools.GenerateFixture COMMAND itch_gen "${fixture}" 20000 10 42)
  add_test(NAME Tools.InspectFixture COMMAND itch_inspect "${fixture}")
  set_tests_properties(Tools.GenerateFixture PROPERTIES FIXTURES_SETUP itch_fixture)
  set_tests_properties(Tools.InspectFixture PROPERTIES FIXTURES_REQUIRED itch_fixture)
endif()
