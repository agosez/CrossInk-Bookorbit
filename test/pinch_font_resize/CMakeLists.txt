add_executable(PinchFontResizeTest
  PinchFontResizeTest.cpp
)

target_include_directories(PinchFontResizeTest PRIVATE
  ${REPO_ROOT}
  ${REPO_ROOT}/src
)

target_link_libraries(PinchFontResizeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PinchFontResizeTest)
