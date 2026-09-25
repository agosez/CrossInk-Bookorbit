add_executable(StablePageNavigationTest
  StablePageNavigationTest.cpp
)

target_include_directories(StablePageNavigationTest PRIVATE
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(StablePageNavigationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(StablePageNavigationTest)
