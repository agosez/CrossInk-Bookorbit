add_executable(OtaStagingGuardTest
  OtaStagingGuardTest.cpp
)

target_compile_definitions(OtaStagingGuardTest PRIVATE
  OTA_UPDATER_SOURCE_PATH="${REPO_ROOT}/src/network/OtaUpdater.cpp"
  FIRMWARE_FLASHER_SOURCE_PATH="${REPO_ROOT}/src/network/FirmwareFlasher.cpp"
)

target_link_libraries(OtaStagingGuardTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OtaStagingGuardTest)
