add_executable(TouchReaderPreviewModelTest
  TouchReaderPreviewModelTest.cpp
)

target_include_directories(TouchReaderPreviewModelTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(TouchReaderPreviewModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TouchReaderPreviewModelTest)
