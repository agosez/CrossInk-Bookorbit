add_executable(QuickLockStateTest
  QuickLockStateTest.cpp
)

target_include_directories(QuickLockStateTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(QuickLockStateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(QuickLockStateTest)
