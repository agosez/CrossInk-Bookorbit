add_executable(FirmwareBoardTagTest
  FirmwareBoardTagTest.cpp
  ${REPO_ROOT}/src/network/FirmwareBoardTag.cpp
)

target_include_directories(FirmwareBoardTagTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/network
)

target_compile_definitions(FirmwareBoardTagTest PRIVATE
  FREEINK_DEVICE_STICKY=1
)

target_link_libraries(FirmwareBoardTagTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FirmwareBoardTagTest)
