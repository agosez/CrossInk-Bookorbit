add_executable(FirmwareVersionTest
  FirmwareVersionTest.cpp
  ${REPO_ROOT}/src/network/FirmwareVersion.cpp
)

target_include_directories(FirmwareVersionTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(FirmwareVersionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FirmwareVersionTest)
