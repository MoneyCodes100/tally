cmake_minimum_required(VERSION 3.15)
project(tally LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# pip (scikit-build) defines SKBUILD and only needs the extension module.
if(DEFINED SKBUILD)
  set(_tally_python_default ON)
  set(_tally_tests_default OFF)
else()
  set(_tally_python_default OFF)
  set(_tally_tests_default ON)
endif()

option(TALLY_BUILD_PYTHON "Build the pybind11 module" ${_tally_python_default})
option(TALLY_BUILD_TESTS "Build C++ tests and the example binary" ${_tally_tests_default})

if(NOT TALLY_BUILD_PYTHON AND NOT TALLY_BUILD_TESTS)
  message(FATAL_ERROR "Enable TALLY_BUILD_PYTHON or TALLY_BUILD_TESTS")
endif()

message(STATUS "tally: python=${TALLY_BUILD_PYTHON} tests=${TALLY_BUILD_TESTS}")

if(TALLY_BUILD_PYTHON)
  set(PYBIND11_FINDPYTHON ON)
  find_package(pybind11 CONFIG REQUIRED)
  pybind11_add_module(_tally MODULE src/tally/_tally.cpp)
  target_include_directories(_tally PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(_tally PRIVATE -Wall -Wextra -Wpedantic -Werror)
  install(TARGETS _tally LIBRARY DESTINATION tally)
endif()

if(TALLY_BUILD_TESTS)
  enable_testing()

  add_executable(tally_tests tests/cpp/test_tally.cpp)
  target_include_directories(tally_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(tally_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME tally_tests COMMAND tally_tests)

  add_executable(tally_example examples/cardinality.cpp)
  target_include_directories(tally_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_compile_options(tally_example PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(
    NAME tally_example
    COMMAND tally_example ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/words.txt
  )
endif()
