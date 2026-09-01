add_executable(ReaderDrawerModelTest
  ReaderDrawerModelTest.cpp
)

target_include_directories(ReaderDrawerModelTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/src/activities/util
)

target_link_libraries(ReaderDrawerModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderDrawerModelTest)
