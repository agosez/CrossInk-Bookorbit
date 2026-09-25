add_executable(HttpRedirectPolicyTest
  HttpRedirectPolicyTest.cpp
  ${REPO_ROOT}/src/util/UrlUtils.cpp
)

target_include_directories(HttpRedirectPolicyTest PRIVATE
  ${REPO_ROOT}/src/network
  ${REPO_ROOT}/src/util
)

target_link_libraries(HttpRedirectPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HttpRedirectPolicyTest)
