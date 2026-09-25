add_executable(DictionaryWordPartsTest
  DictionaryWordPartsTest.cpp
)

target_include_directories(DictionaryWordPartsTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(DictionaryWordPartsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictionaryWordPartsTest)
