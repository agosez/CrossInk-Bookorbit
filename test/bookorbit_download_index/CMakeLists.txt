add_executable(BookOrbitDownloadIndexTest
  BookOrbitDownloadIndexTest.cpp
  ${REPO_ROOT}/src/BookOrbitDownloadIndex.cpp
)

target_include_directories(BookOrbitDownloadIndexTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(BookOrbitDownloadIndexTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitDownloadIndexTest)
