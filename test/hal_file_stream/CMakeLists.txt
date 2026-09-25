add_executable(HalFileStreamTest HalFileStreamTest.cpp)
target_include_directories(HalFileStreamTest PRIVATE stubs ${REPO_ROOT}/lib/hal)
target_link_libraries(HalFileStreamTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalFileStreamTest)
