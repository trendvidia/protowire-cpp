add_executable(bench_pxf main.cc)
set_target_properties(bench_pxf PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_link_libraries(bench_pxf PRIVATE protowire_pxf)
