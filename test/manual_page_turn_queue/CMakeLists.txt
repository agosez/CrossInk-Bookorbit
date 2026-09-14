add_executable(ManualPageTurnQueueTest
  ManualPageTurnQueueTest.cpp
)

target_include_directories(ManualPageTurnQueueTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ManualPageTurnQueueTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ManualPageTurnQueueTest)
