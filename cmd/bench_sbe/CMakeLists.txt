add_executable(bench_sbe main.cc)
set_target_properties(bench_sbe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_link_libraries(bench_sbe PRIVATE protowire_sbe)
