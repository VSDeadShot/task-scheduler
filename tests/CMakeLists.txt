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
  gtest_discover_tests(${name} PROPERTIES TIMEOUT 60)
endfunction()

tsched_add_test(thread_pool_lifecycle_test thread_pool_lifecycle_test.cpp)
tsched_add_test(work_stealing_deque_test work_stealing_deque_test.cpp)
tsched_add_test(task_test task_test.cpp)
tsched_add_test(thread_pool_submit_test thread_pool_submit_test.cpp)
