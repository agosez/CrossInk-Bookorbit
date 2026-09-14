add_executable(BufferedFileTest BufferedFileTest.cpp)
target_include_directories(BufferedFileTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
)
target_link_libraries(BufferedFileTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BufferedFileTest)
