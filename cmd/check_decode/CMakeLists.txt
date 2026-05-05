add_executable(check_decode main.cc)
set_target_properties(check_decode PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_link_libraries(check_decode PRIVATE protowire_pb protowire_pxf)
