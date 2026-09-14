add_executable(ReaderStatusBarTapTargetTest
  ReaderStatusBarTapTargetTest.cpp
)

target_include_directories(ReaderStatusBarTapTargetTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ReaderStatusBarTapTargetTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderStatusBarTapTargetTest)
