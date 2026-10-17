cmake_minimum_required(VERSION 3.14)
project(sflat_flat_hash_map CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Header-only library
add_library(sflat INTERFACE)
target_include_directories(sflat INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# SIMD: let the compiler pick the best available (or set your own -march)
if(NOT CMAKE_CXX_FLAGS MATCHES "march=")
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-march=native" HAS_MARCH_NATIVE)
  if(HAS_MARCH_NATIVE)
    target_compile_options(sflat INTERFACE -march=native)
  endif()
endif()

option(SFLAT_BUILD_TESTS "Build correctness tests" ON)
option(SFLAT_BUILD_BENCH "Build benchmark (needs thirdparty/)" ON)

if(SFLAT_BUILD_TESTS)
  add_executable(correctness tests/correctness.cpp)
  target_link_libraries(correctness PRIVATE sflat)
  target_compile_options(correctness PRIVATE -O2 -Wall)
  add_executable(growth_tiers tests/growth_tiers.cpp)
  target_link_libraries(growth_tiers PRIVATE sflat)
  target_compile_options(growth_tiers PRIVATE -O2 -Wall)
endif()

if(SFLAT_BUILD_BENCH)
  # emhash: thirdparty/emhash (old flat layout) or thirdparty/emhash/include
  # (current upstream layout). Boost: thirdparty/boost/include or system.
  find_path(SFLAT_EMHASH_DIR emhash/hash_table8.hpp
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/emhash
          ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/emhash/include
    NO_DEFAULT_PATH)
  find_path(SFLAT_BOOST_DIR boost/unordered/unordered_flat_map.hpp
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/boost/include)
  if(SFLAT_EMHASH_DIR AND SFLAT_BOOST_DIR)
    add_executable(bench bench/bench.cpp)
    target_link_libraries(bench PRIVATE sflat)
    target_include_directories(bench PRIVATE ${SFLAT_BOOST_DIR}
                                             ${SFLAT_EMHASH_DIR})
    target_compile_options(bench PRIVATE -O3 -Wall)
    # Release semantics for all maps (no debug asserts in absl or boost).
    target_compile_definitions(bench PRIVATE NDEBUG)
    # Optional Abseil: thirdparty/abseil (an install prefix) or the system.
    find_package(absl CONFIG QUIET
      PATHS ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/abseil)
    if(absl_FOUND)
      target_compile_definitions(bench PRIVATE HAS_ABSL)
      target_link_libraries(bench PRIVATE absl::flat_hash_map
                                          absl::node_hash_map)
    endif()
    # Optional Google sparsehash (header only): thirdparty/sparsehash/include
    # or the system.
    find_path(SFLAT_SPARSEHASH_DIR sparsehash/dense_hash_map
      PATHS ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/sparsehash/include)
    if(SFLAT_SPARSEHASH_DIR)
      target_compile_definitions(bench PRIVATE HAS_SPARSEHASH)
      target_include_directories(bench PRIVATE ${SFLAT_SPARSEHASH_DIR})
    endif()
    # Optional tsl::robin_map (header only): thirdparty/tsl-robin-map.
    find_path(SFLAT_TSL_DIR tsl/robin_map.h
      PATHS ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/tsl-robin-map/include)
    if(SFLAT_TSL_DIR)
      target_compile_definitions(bench PRIVATE HAS_TSL)
      target_include_directories(bench PRIVATE ${SFLAT_TSL_DIR})
    endif()
    # Optional ska::flat_hash_map (header only): thirdparty/ska.
    find_path(SFLAT_SKA_DIR flat_hash_map.hpp
      PATHS ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/ska NO_DEFAULT_PATH)
    if(SFLAT_SKA_DIR)
      target_compile_definitions(bench PRIVATE HAS_SKA)
      target_include_directories(bench PRIVATE ${SFLAT_SKA_DIR})
    endif()
    # Optional folly F14: the folly sources in thirdparty/folly-src and fmt in
    # thirdparty/fmt. F14 needs only three folly .cpp files, so folly is not
    # built or installed. Current folly needs C++20, so with folly the bench
    # is built as C++20.
    set(SFLAT_FOLLY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/folly-src)
    set(SFLAT_FMT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/fmt/include)
    if(EXISTS ${SFLAT_FOLLY_DIR}/folly/container/F14Map.h AND
       EXISTS ${SFLAT_FMT_DIR}/fmt/format.h)
      set(cfg ${CMAKE_CURRENT_BINARY_DIR}/folly-config/folly/folly-config.h)
      file(WRITE ${cfg} "#pragma once\n"
        "#define FOLLY_HAVE_PTHREAD 1\n"
        "#define FOLLY_HAVE_LIBGFLAGS 0\n"
        "#define FOLLY_HAVE_LIBGLOG 0\n"
        "#define FOLLY_HAVE_WEAK_SYMBOLS 1\n"
        "#define FOLLY_HAVE_MALLOC_USABLE_SIZE 1\n"
        "#define FOLLY_HAVE_INT128_T 1\n"
        "#define FOLLY_HAVE_UNALIGNED_ACCESS 1\n"
        "#define FOLLY_HAVE_VLA 1\n"
        "#define FOLLY_SUPPLY_MISSING_INT128_TRAITS 0\n")
      target_sources(bench PRIVATE
        ${SFLAT_FOLLY_DIR}/folly/container/detail/F14Table.cpp
        ${SFLAT_FOLLY_DIR}/folly/lang/SafeAssert.cpp
        ${SFLAT_FOLLY_DIR}/folly/lang/ToAscii.cpp)
      target_compile_definitions(bench PRIVATE HAS_FOLLY FMT_HEADER_ONLY)
      target_include_directories(bench PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/folly-config ${SFLAT_FOLLY_DIR}
        ${SFLAT_FMT_DIR})
      set_target_properties(bench PROPERTIES CXX_STANDARD 20)
    endif()
  else()
    message(STATUS "emhash or boost not found, skipping bench target")
  endif()
endif()
