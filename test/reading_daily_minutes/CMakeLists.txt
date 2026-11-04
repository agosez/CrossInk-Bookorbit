add_executable(ReadingDailyMinutesTest
  ReadingDailyMinutesTest.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingDailyMinutes.cpp
  ${REPO_ROOT}/src/activities/reader/ReadingStatsUtils.cpp
)

target_include_directories(ReadingDailyMinutesTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ReadingDailyMinutesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingDailyMinutesTest)
