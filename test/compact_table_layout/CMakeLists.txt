add_executable(CompactTableLayoutTest
  CompactTableLayoutTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/tables/CompactTableLayout.cpp
)

target_include_directories(CompactTableLayoutTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Epub/Epub/tables
)

target_compile_definitions(CompactTableLayoutTest PRIVATE
  COMPACT_TABLE_FIXTURE_PATH="${REPO_ROOT}/test/epubs-src/test_compact_table_low_memory/OEBPS/content.xhtml"
)

target_link_libraries(CompactTableLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompactTableLayoutTest)
