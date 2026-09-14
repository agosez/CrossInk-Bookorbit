add_executable(ReleaseSuppressionTest
  ReleaseSuppressionTest.cpp
)

target_include_directories(ReleaseSuppressionTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ReleaseSuppressionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReleaseSuppressionTest)
