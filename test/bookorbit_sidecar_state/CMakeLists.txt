add_executable(BookOrbitSidecarStateTest
  BookOrbitSidecarStateTest.cpp
)

target_link_libraries(BookOrbitSidecarStateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitSidecarStateTest)
