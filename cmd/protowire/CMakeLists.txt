add_executable(protowire_cli main.cc)
set_target_properties(protowire_cli PROPERTIES OUTPUT_NAME protowire)
target_include_directories(protowire_cli PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)
target_compile_definitions(protowire_cli PRIVATE
  PROTOWIRE_WKT_PROTO_DIR="${Protobuf_INCLUDE_DIR}")
target_link_libraries(protowire_cli PRIVATE
  protowire_pxf
  protowire_sbe
  protobuf::libprotoc
  protobuf::libprotobuf)

if(PROTOWIRE_WITH_REGISTRY)
  find_package(gRPC REQUIRED CONFIG)
  # Generate gRPC stubs for the protoregistry service (client side only).
  set(REG_PROTO_DIR ${CMAKE_CURRENT_BINARY_DIR}/registry_gen)
  file(MAKE_DIRECTORY ${REG_PROTO_DIR})
  add_library(protowire_registry_stubs STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../proto/protoregistry/v1/registry.proto)
  target_include_directories(protowire_registry_stubs PUBLIC
    $<BUILD_INTERFACE:${REG_PROTO_DIR}>)
  target_link_libraries(protowire_registry_stubs PUBLIC
    protobuf::libprotobuf gRPC::grpc++)
  protobuf_generate(
    TARGET protowire_registry_stubs
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../../proto
    PROTOC_OUT_DIR ${REG_PROTO_DIR})
  protobuf_generate(
    TARGET protowire_registry_stubs
    LANGUAGE grpc
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN "protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../../proto
    PROTOC_OUT_DIR ${REG_PROTO_DIR})
  target_link_libraries(protowire_cli PRIVATE
    protowire_registry_stubs gRPC::grpc++)
  target_compile_definitions(protowire_cli PRIVATE PROTOWIRE_WITH_REGISTRY=1)
endif()
