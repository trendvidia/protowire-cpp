cmake_minimum_required(VERSION 3.20)
project(protowire VERSION 0.71.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Imported targets (e.g. protobuf via Homebrew) sometimes export their
# headers as -isystem, which on Apple-clang loses precedence to a stale
# /usr/local/include. Forcing user-style -I keeps Homebrew's headers first.
set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)

option(PROTOWIRE_BUILD_TESTS "Build unit tests" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# Some macOS systems still have an old protobuf at /usr/local/include that
# clang adds to its default search path before any -isystem path. Force
# /opt/homebrew/include to be searched first (only if it exists) so that
# Homebrew's protobuf headers win.
if(APPLE AND EXISTS /opt/homebrew/include AND EXISTS /usr/local/include/google/protobuf)
  add_compile_options(-I/opt/homebrew/include)
  message(STATUS "Detected stale /usr/local/include/google/protobuf — "
                 "prepending /opt/homebrew/include to compile search path.")
endif()

# --- Helper: only-if-files-exist source list -------------------------------
function(_pwx_existing_sources out_var)
  set(found "")
  foreach(p ${ARGN})
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${p}")
      list(APPEND found "${p}")
    endif()
  endforeach()
  set(${out_var} "${found}" PARENT_SCOPE)
endfunction()

# --- Foundation -----------------------------------------------------------
add_library(protowire_detail STATIC
  src/detail/base64.cc
  src/detail/rfc3339.cc
  src/detail/duration.cc
  src/detail/status.cc)
target_include_directories(protowire_detail PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# --- Optional package libraries (only built when their .cc files exist) ----
_pwx_existing_sources(_pb_srcs src/pb/wire.cc src/pb/big.cc)
if(_pb_srcs)
  add_library(protowire_pb STATIC ${_pb_srcs})
  target_include_directories(protowire_pb PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
  target_link_libraries(protowire_pb PUBLIC protowire_detail)
endif()

_pwx_existing_sources(_env_srcs src/envelope/envelope.cc)
if(_env_srcs AND TARGET protowire_pb)
  add_library(protowire_envelope STATIC ${_env_srcs})
  target_link_libraries(protowire_envelope PUBLIC protowire_pb)
endif()

# protobuf is only required for pxf, sbe, and the CLI.
_pwx_existing_sources(_pxf_srcs
  src/pxf/lexer.cc src/pxf/ast.cc src/pxf/parser.cc src/pxf/decode_fast.cc
  src/pxf/decode.cc src/pxf/encode.cc src/pxf/format.cc src/pxf/wellknown.cc
  src/pxf/annotations.cc src/pxf/result.cc src/pxf/options.cc)
_pwx_existing_sources(_sbe_srcs
  src/sbe/annotations.cc src/sbe/template.cc src/sbe/codec.cc
  src/sbe/marshal.cc src/sbe/unmarshal.cc src/sbe/view.cc
  src/sbe/xmlschema.cc src/sbe/xmltoproto.cc src/sbe/prototoxml.cc)

if(_pxf_srcs OR _sbe_srcs)
  # Prefer the CONFIG package (Homebrew, vcpkg, source builds ship
  # protobuf-config.cmake and bring abseil deps along). Fall back to the
  # FindProtobuf module shipped with CMake (Ubuntu's libprotobuf-dev does
  # not always install protobuf-config.cmake). Both expose the imported
  # target protobuf::libprotobuf and the protobuf_generate() command
  # (FindProtobuf gained protobuf_generate in CMake 3.27).
  find_package(Protobuf QUIET CONFIG)
  if(NOT Protobuf_FOUND)
    find_package(Protobuf REQUIRED)
  endif()

  # Homebrew's protobuf CONFIG exports PROTOBUF_USE_DLLS as an interface
  # compile def, which breaks compilation against the static-built install.
  # Strip it from the imported target.
  if(TARGET protobuf::libprotobuf)
    get_target_property(_pb_defs protobuf::libprotobuf
                        INTERFACE_COMPILE_DEFINITIONS)
    if(_pb_defs)
      list(REMOVE_ITEM _pb_defs PROTOBUF_USE_DLLS)
      set_target_properties(protobuf::libprotobuf PROPERTIES
        INTERFACE_COMPILE_DEFINITIONS "${_pb_defs}")
    endif()
    # Force protobuf includes to be -I (not -isystem) so they win over any
    # stale /usr/local/include/google/protobuf left behind on this machine.
    set_property(TARGET protobuf::libprotobuf PROPERTY
                 IMPORTED_NO_SYSTEM TRUE)
    set_property(TARGET protobuf::libprotobuf PROPERTY SYSTEM FALSE)
  endif()
  if(TARGET protobuf::libprotoc)
    set_property(TARGET protobuf::libprotoc PROPERTY
                 IMPORTED_NO_SYSTEM TRUE)
    set_property(TARGET protobuf::libprotoc PROPERTY SYSTEM FALSE)
  endif()

  # Well-known type .proto files (timestamp.proto, duration.proto, etc.)
  # ship next to libprotobuf — derive their location from the imported target
  # so tests and the CLI work on any host that has libprotobuf installed.
  if(NOT DEFINED Protobuf_INCLUDE_DIR OR Protobuf_INCLUDE_DIR STREQUAL "")
    get_target_property(Protobuf_INCLUDE_DIR protobuf::libprotobuf
                        INTERFACE_INCLUDE_DIRECTORIES)
    # INTERFACE_INCLUDE_DIRECTORIES may be a list — pick the first entry.
    list(GET Protobuf_INCLUDE_DIR 0 Protobuf_INCLUDE_DIR)
  endif()

  set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
  file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

  set(PROTOWIRE_PROTO_FILES
    proto/pxf/annotations.proto
    proto/pxf/bignum.proto
    proto/sbe/annotations.proto
    proto/envelope/v1/envelope.proto)

  add_library(protowire_protos STATIC ${PROTOWIRE_PROTO_FILES})
  target_include_directories(protowire_protos PUBLIC
    $<BUILD_INTERFACE:${PROTO_GEN_DIR}>)
  target_link_libraries(protowire_protos PUBLIC protobuf::libprotobuf)
  protobuf_generate(
    TARGET protowire_protos
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/proto
    PROTOC_OUT_DIR ${PROTO_GEN_DIR})
endif()

if(_pxf_srcs)
  add_library(protowire_pxf STATIC ${_pxf_srcs})
  target_include_directories(protowire_pxf PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROTO_GEN_DIR}>)
  target_link_libraries(protowire_pxf PUBLIC
    protowire_detail protobuf::libprotobuf protowire_protos)
  if(TARGET protowire_pb)
    target_link_libraries(protowire_pxf PUBLIC protowire_pb)
  endif()
endif()

if(_sbe_srcs)
  find_package(tinyxml2 QUIET CONFIG)
  if(NOT tinyxml2_FOUND)
    include(FetchContent)
    FetchContent_Declare(tinyxml2
      GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
      GIT_TAG 10.0.0)
    set(tinyxml2_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(tinyxml2)
  endif()

  add_library(protowire_sbe STATIC ${_sbe_srcs})
  target_include_directories(protowire_sbe PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROTO_GEN_DIR}>)
  target_link_libraries(protowire_sbe PUBLIC
    protowire_detail protobuf::libprotobuf protowire_protos tinyxml2::tinyxml2)
endif()

# Cross-port envelope wire-compat dumper. Cheap to build (links only against
# the envelope/pb static libs) so always included when those libs are built.
if(TARGET protowire_envelope AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/cmd/dump_envelope/main.cc)
  add_subdirectory(cmd/dump_envelope)
endif()

# Cross-port PXF microbench. Same shape as the Go/Rust/TS/Java bench-pxf
# binaries; output is aggregated by `protowire/scripts/cross_pxf_bench.sh`.
if(TARGET protowire_pxf AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/cmd/bench_pxf/main.cc)
  add_subdirectory(cmd/bench_pxf)
endif()

# Cross-port SBE microbench. Aggregated by `protowire/scripts/cross_sbe_bench.sh`.
if(TARGET protowire_sbe AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/cmd/bench_sbe/main.cc)
  add_subdirectory(cmd/bench_sbe)
endif()

# HARDENING.md conformance corpus harness. Aggregated by
# `protowire/scripts/cross_security_check.sh`. See ROADMAP M8.
if(TARGET protowire_pxf AND TARGET protowire_pb
   AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/cmd/check_decode/main.cc)
  add_subdirectory(cmd/check_decode)
endif()

if(PROTOWIRE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()
