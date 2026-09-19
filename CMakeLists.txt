cmake_minimum_required(VERSION 3.24)
project(tsched VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TSCHED_BUILD_TESTS "Build the tsched test suite" ON)
set(TSCHED_SANITIZER "" CACHE STRING "Sanitizer to build with: thread, address, or empty for none")
set_property(CACHE TSCHED_SANITIZER PROPERTY STRINGS "" thread address)

# Sanitizers apply to every target (including GoogleTest): TSan needs the whole
# process instrumented, or it reports false positives on uninstrumented code.
if(TSCHED_SANITIZER STREQUAL "thread" OR TSCHED_SANITIZER STREQUAL "address")
  add_compile_options(-fsanitize=${TSCHED_SANITIZER} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${TSCHED_SANITIZER})
elseif(NOT TSCHED_SANITIZER STREQUAL "")
  message(FATAL_ERROR "TSCHED_SANITIZER must be 'thread', 'address', or empty; got '${TSCHED_SANITIZER}'")
endif()

# Warnings-as-errors for our own targets only; third-party code is not ours to police.
add_library(tsched_warnings INTERFACE)
target_compile_options(tsched_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)

find_package(Threads REQUIRED)

add_library(tsched src/thread_pool.cpp)
add_library(tsched::tsched ALIAS tsched)
target_include_directories(tsched PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(tsched
  PUBLIC Threads::Threads
  PRIVATE tsched_warnings)

if(TSCHED_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
