find_package(EXPAT REQUIRED)
add_executable(HttpDownloaderTest
  HttpDownloaderTest.cpp
  ${REPO_ROOT}/lib/OpdsParser/OpdsParser.cpp
  ${REPO_ROOT}/src/network/HttpDownloader.cpp
  ${REPO_ROOT}/src/util/UrlUtils.cpp
)
target_include_directories(HttpDownloaderTest PRIVATE
  stubs
  ${REPO_ROOT}/test/opds_parser/stubs
  ${REPO_ROOT}/lib/OpdsParser
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/network ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/src/util
)
target_compile_definitions(HttpDownloaderTest PRIVATE SIMULATOR FREEINK_NET_WOLFSSL=1)
target_link_libraries(HttpDownloaderTest PRIVATE crosspoint_test_common EXPAT::EXPAT GTest::gtest_main)
gtest_discover_tests(HttpDownloaderTest)

# Compile the actual device-only free-space preflight on the host as well.
# The existing simulator target intentionally excludes this code path.
add_executable(HttpDownloaderDevicePathTest ${REPO_ROOT}/lib/OpdsParser/OpdsParser.cpp HttpDownloaderTest.cpp ${REPO_ROOT}/src/network/HttpDownloader.cpp ${REPO_ROOT}/src/util/UrlUtils.cpp)
target_include_directories(HttpDownloaderDevicePathTest PRIVATE stubs ${REPO_ROOT}/test/opds_parser/stubs ${REPO_ROOT}/lib/OpdsParser ${REPO_ROOT}/lib/XmlParserUtils ${REPO_ROOT}/src ${REPO_ROOT}/src/network ${REPO_ROOT}/lib/FsHelpers ${REPO_ROOT}/src/util)
target_compile_definitions(HttpDownloaderDevicePathTest PRIVATE FREEINK_NET_WOLFSSL=1)
target_link_libraries(HttpDownloaderDevicePathTest PRIVATE EXPAT::EXPAT crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HttpDownloaderDevicePathTest TEST_SUFFIX .DevicePath)
