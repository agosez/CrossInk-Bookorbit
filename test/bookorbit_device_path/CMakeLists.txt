add_executable(BookOrbitDevicePathTest
  BookOrbitDevicePathTest.cpp
  ${REPO_ROOT}/src/util/BookOrbitDevicePath.cpp
  ${REPO_ROOT}/src/util/StringUtils.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(BookOrbitDevicePathTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(BookOrbitDevicePathTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitDevicePathTest)
