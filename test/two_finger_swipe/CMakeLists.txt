add_executable(TwoFingerSwipeTest
  TwoFingerSwipeTest.cpp
)

target_include_directories(TwoFingerSwipeTest PRIVATE
  ${REPO_ROOT}
  ${REPO_ROOT}/src
)

target_link_libraries(TwoFingerSwipeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TwoFingerSwipeTest)
