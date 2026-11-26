find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(WOLFSSL IMPORTED_TARGET wolfssl)
endif()
if(NOT WOLFSSL_FOUND)
  message(STATUS "wolfSSL not found: skipping AssetCatalogSignatureTest")
  return()
endif()

add_executable(AssetCatalogSignatureTest AssetCatalogSignatureTest.cpp
  ${REPO_ROOT}/src/network/AssetCatalogSignature.cpp)
target_include_directories(AssetCatalogSignatureTest PRIVATE
  ${REPO_ROOT}/src/network ${REPO_ROOT}/lib/AssetCatalog ${REPO_ROOT}/lib/Memory)
# SIMULATOR also covers the smoke test's CROSSINK_SIM_ASSET_CATALOG_KEY.
target_compile_definitions(AssetCatalogSignatureTest PRIVATE SIMULATOR)
target_compile_options(AssetCatalogSignatureTest PRIVATE -include wolfssl/options.h)
target_link_libraries(AssetCatalogSignatureTest PRIVATE PkgConfig::WOLFSSL GTest::gtest_main)
gtest_discover_tests(AssetCatalogSignatureTest)
