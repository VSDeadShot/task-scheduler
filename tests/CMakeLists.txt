include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
  URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(thread_pool_lifecycle_test thread_pool_lifecycle_test.cpp)
target_link_libraries(thread_pool_lifecycle_test
  PRIVATE tsched::tsched GTest::gtest_main tsched_warnings)

include(GoogleTest)
# A deadlocked test (e.g. a hang inside stop()) can't fail on its own: joining a
# stuck thread just blocks. The timeout makes ctest kill it and report a failure.
gtest_discover_tests(thread_pool_lifecycle_test PROPERTIES TIMEOUT 60)
