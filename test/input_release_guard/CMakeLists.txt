add_executable(InputReleaseGuardTest
  InputReleaseGuardTest.cpp
)

target_include_directories(InputReleaseGuardTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
)

target_link_libraries(InputReleaseGuardTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(InputReleaseGuardTest)
