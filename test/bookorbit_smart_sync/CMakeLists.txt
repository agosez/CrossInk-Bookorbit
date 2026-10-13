add_executable(BookOrbitSmartSyncTest
  BookOrbitSmartSyncTest.cpp
)

target_include_directories(BookOrbitSmartSyncTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(BookOrbitSmartSyncTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitSmartSyncTest)
