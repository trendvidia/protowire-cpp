include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(protowire_detail_test
  detail_base64_test.cc
  detail_rfc3339_test.cc
  detail_duration_test.cc)
target_link_libraries(protowire_detail_test PRIVATE
  protowire_detail GTest::gtest GTest::gtest_main)
gtest_discover_tests(protowire_detail_test)

if(TARGET protowire_pb AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/pb_test.cc)
  add_executable(protowire_pb_test pb_test.cc)
  target_link_libraries(protowire_pb_test PRIVATE
    protowire_pb GTest::gtest GTest::gtest_main)
  gtest_discover_tests(protowire_pb_test)
endif()

if(TARGET protowire_envelope AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/envelope_test.cc)
  add_executable(protowire_envelope_test envelope_test.cc)
  target_link_libraries(protowire_envelope_test PRIVATE
    protowire_envelope GTest::gtest GTest::gtest_main)
  gtest_discover_tests(protowire_envelope_test)
endif()

if(TARGET protowire_pxf)
  set(_pxf_test_srcs "")
  foreach(f pxf_lexer_test pxf_parser_test pxf_decode_test pxf_encode_test
            pxf_null_test pxf_bignum_test pxf_features_test pxf_any_test
            pxf_fast_test pxf_full_roundtrip_test pxf_escapes_test
            pxf_annotations_test pxf_format_test pxf_directive_test
            pxf_schema_test pxf_result_directives_test)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${f}.cc)
      list(APPEND _pxf_test_srcs ${f}.cc)
    endif()
  endforeach()
  if(_pxf_test_srcs)
    add_executable(protowire_pxf_test ${_pxf_test_srcs})
    target_link_libraries(protowire_pxf_test PRIVATE
      protowire_pxf protobuf::libprotoc
      GTest::gtest GTest::gtest_main)
    target_compile_definitions(protowire_pxf_test PRIVATE
      TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../testdata"
      PROTO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../proto"
      WKT_PROTO_DIR="${Protobuf_INCLUDE_DIR}")
    gtest_discover_tests(protowire_pxf_test)
  endif()
endif()

if(TARGET protowire_sbe)
  set(_sbe_test_srcs "")
  foreach(f sbe_codec_test sbe_convert_test sbe_navigation_test)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${f}.cc)
      list(APPEND _sbe_test_srcs ${f}.cc)
    endif()
  endforeach()
  if(_sbe_test_srcs)
    add_executable(protowire_sbe_test ${_sbe_test_srcs})
    target_link_libraries(protowire_sbe_test PRIVATE
      protowire_sbe protobuf::libprotoc
      GTest::gtest GTest::gtest_main)
    target_compile_definitions(protowire_sbe_test PRIVATE
      PROTO_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../proto"
      WKT_PROTO_DIR="${Protobuf_INCLUDE_DIR}")
    gtest_discover_tests(protowire_sbe_test)
  endif()
endif()
