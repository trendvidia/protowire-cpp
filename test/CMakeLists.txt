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
  add_executable(protowire_pb_test pb_test.cc pb_stream_test.cc pb_utf8_test.cc pb_values_table_test.cc)
  target_link_libraries(protowire_pb_test PRIVATE
    protowire_pb GTest::gtest GTest::gtest_main)
  target_compile_definitions(protowire_pb_test PRIVATE
    TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../testdata")
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
  foreach(f pxf_lexer_test pxf_lexer_duration_test pxf_corpus_test pxf_carrier_test pxf_parser_test pxf_decode_test pxf_encode_test
            pxf_null_test pxf_bignum_test pxf_features_test pxf_any_test
            pxf_fast_test pxf_full_roundtrip_test pxf_escapes_test
            pxf_annotations_test pxf_format_test pxf_tolerant_test pxf_check_test pxf_directive_test
            pxf_schema_test pxf_schema_rules_test pxf_secret_test pxf_element_defaults_test pxf_result_directives_test pxf_dataset_reader_test
            pxf_proto_directive_test pxf_map_keys_test pxf_keyed_test pxf_hardening_test pxf_go_parity_test pxf_dataset_writer_test pxf_rewriter_test pxf_wkt_shape_test
            pxf_nan_test pxf_format_directives_test
            pxf_duration_block_test
            pxf_marshal_depth_test
            pxf_marshal_unset_annotated_test)
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
  foreach(f sbe_codec_test sbe_convert_test sbe_navigation_test sbe_stream_test
            sbe_template_id_test sbe_header_bounds_test sbe_encoding_range_test
            sbe_oneof_test)
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

# check_decode prints a PXF rejection with its position, as protowire-go's
# does (#145). The spec's gates compare these lines across ports.
if(TARGET check_decode)
  foreach(fmt pxf pxf-pb)
    add_test(NAME check_decode_position_${fmt}
      COMMAND check_decode --format ${fmt} --schema test.v1.AllTypes
        --proto ${CMAKE_CURRENT_SOURCE_DIR}/../testdata/test.proto
        --input ${CMAKE_CURRENT_SOURCE_DIR}/../testdata/check_decode/unknown-field.pxf)
    set_tests_properties(check_decode_position_${fmt} PROPERTIES
      PASS_REGULAR_EXPRESSION "reject: 3:3: unknown field \"nope\" in test\\.v1\\.AllTypes")
  endforeach()
endif()
