add_executable(SectionPageIndexTest
  SectionPageIndexTest.cpp
  SectionPageIndexPersistenceTest.cpp
)

target_link_libraries(SectionPageIndexTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(SectionPageIndexTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
)

gtest_discover_tests(SectionPageIndexTest)
