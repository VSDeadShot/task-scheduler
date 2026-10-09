include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
  URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# Every test executable goes through this so none can skip the timeout.
# A deadlocked test (e.g. a hang inside stop()) can't fail on its own: joining a
# stuck thread just blocks. The timeout makes ctest kill it and report a failure.
# Sanitizer flags come from the top-level TSCHED_SANITIZER, which applies to all targets.
function(tsched_add_test name)
  if(NOT ARGN)
    message(FATAL_ERROR "tsched_add_test(${name}): no source files given")
  endif()
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE tsched::tsched GTest::gtest_main tsched_warnings)
  # NO_PRETTY_VALUES: name parameterized tests by GoogleTest's own names (e.g.
  # .../StealingOn) rather than by the raw parameter value (.../true).
  gtest_discover_tests(${name} NO_PRETTY_VALUES PROPERTIES TIMEOUT 60)
endfunction()

tsched_add_test(thread_pool_lifecycle_test thread_pool_lifecycle_test.cpp)
tsched_add_test(work_stealing_deque_test work_stealing_deque_test.cpp)
tsched_add_test(task_test task_test.cpp)
tsched_add_test(thread_pool_submit_test thread_pool_submit_test.cpp)

# fail_alloc.cpp replaces the global operator new, so it gets an executable of
# its own: no other test should run with a replaced allocator.
tsched_add_test(thread_pool_alloc_failure_test thread_pool_alloc_failure_test.cpp fail_alloc.cpp)

# Under clang, ThreadSanitizer's runtime is linked statically and whole, and it
# defines operator new itself, so the replacement above fails to link
# ("multiple definition of operator new"). Linking the runtime as a shared
# library instead lets the executable's definitions take precedence, and TSan
# still instruments everything (a deliberate data race is still reported). The
# rpath points at the clang directory holding that shared runtime, which isn't
# on the loader's default search path. Only this target is affected; g++ links
# its TSan runtime as a shared library already. Only the TSan path has been
# exercised: another sanitizer would need the same treatment, checked first.
if(TSCHED_SANITIZER STREQUAL "thread" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libclang_rt.tsan-${CMAKE_SYSTEM_PROCESSOR}.so
    OUTPUT_VARIABLE _tsched_tsan_library
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  # Given a name it can't find, -print-file-name prints the name back unchanged.
  if(NOT IS_ABSOLUTE "${_tsched_tsan_library}" OR NOT EXISTS "${_tsched_tsan_library}")
    message(FATAL_ERROR "shared TSan runtime not found (got '${_tsched_tsan_library}')")
  endif()
  get_filename_component(_tsched_san_dir "${_tsched_tsan_library}" DIRECTORY)
  target_link_options(thread_pool_alloc_failure_test PRIVATE -shared-libsan)
  set_target_properties(thread_pool_alloc_failure_test PROPERTIES BUILD_RPATH "${_tsched_san_dir}")
endif()
