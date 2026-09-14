add_executable(SleepWakePolicyTest
  SleepWakePolicyTest.cpp
)

target_include_directories(SleepWakePolicyTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(SleepWakePolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SleepWakePolicyTest)
