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
  else()
    message(STATUS "emhash or boost not found, skipping bench target")
  endif()
endif()
