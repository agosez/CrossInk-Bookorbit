cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pxc_v2_tests C CXX)
  enable_testing()
endif()
set(PXC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
add_executable(PxcV2Test PxcV2Test.cpp
  ${PXC_ROOT}/lib/Epub/Epub/image/PxcV2.cpp
  ${PXC_ROOT}/lib/InflateReader/InflateReader.cpp
  ${PXC_ROOT}/lib/uzlib/src/tinflate.c
  ${PXC_ROOT}/lib/uzlib/src/uzlib_checksums.c)
target_compile_features(PxcV2Test PRIVATE cxx_std_17)
target_include_directories(PxcV2Test PRIVATE stubs ${PXC_ROOT}/lib/Epub/Epub/image
  ${PXC_ROOT}/lib/InflateReader ${PXC_ROOT}/lib/uzlib/src)
target_compile_definitions(PxcV2Test PRIVATE GOLDEN_DIR="${CMAKE_CURRENT_LIST_DIR}/golden")
add_test(NAME PxcV2Test COMMAND PxcV2Test)

add_executable(StoredZipTest StoredZipTest.cpp ${PXC_ROOT}/lib/ZipFile/ZipFile.cpp)
target_compile_features(StoredZipTest PRIVATE cxx_std_20)
target_include_directories(StoredZipTest PRIVATE stubs ${PXC_ROOT}/lib/ZipFile ${PXC_ROOT}/lib/Memory)
target_compile_definitions(StoredZipTest PRIVATE GOLDEN_DIR="${CMAKE_CURRENT_LIST_DIR}/golden")
add_test(NAME StoredZipTest COMMAND StoredZipTest)

set_tests_properties(PxcV2Test StoredZipTest PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
