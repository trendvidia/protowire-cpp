add_executable(dump_envelope main.cc)
set_target_properties(dump_envelope PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_link_libraries(dump_envelope PRIVATE protowire_envelope)
