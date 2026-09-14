add_executable(PageCountEstimatorTest
  PageCountEstimatorTest.cpp
)

target_link_libraries(PageCountEstimatorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(PageCountEstimatorTest PRIVATE
  ${REPO_ROOT}/lib/Epub/Epub
)

gtest_discover_tests(PageCountEstimatorTest)
