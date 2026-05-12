add_executable(check_decode main.cc)
set_target_properties(check_decode PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# Need libprotoc for Importer + DiskSourceTree (runtime .proto compilation).
# Distros split libprotoc out of libprotobuf-dev (Ubuntu ships it as
# libprotoc-dev). The spec repo's cross_security_check.sh installs both
# in CI.
find_package(Protobuf REQUIRED)
find_library(PROTOC_LIB protoc)
if(NOT PROTOC_LIB)
  message(FATAL_ERROR "libprotoc not found — install libprotoc-dev (or equivalent)")
endif()
# Pin the well-known-types proto dir so the Importer can resolve
# `google/protobuf/{descriptor,timestamp,duration,wrappers}.proto` —
# the adversarial corpus's `sbe/annotations.proto` imports
# `google/protobuf/descriptor.proto`. Same pattern the test target uses.
target_compile_definitions(check_decode PRIVATE
  WKT_PROTO_DIR="${Protobuf_INCLUDE_DIR}")
target_link_libraries(check_decode PRIVATE
  protowire_pxf
  ${PROTOC_LIB}
  protobuf::libprotobuf)
