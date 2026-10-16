# ThreadSanitizer: libstdc++ exception refcount false positive

ThreadSanitizer can report a data race when one thread stores an exception in a
future and another thread catches it from that future and reads it. The program
is correct; TSan just can't see the ordering. This directory holds a minimal
reproduction, and the rule for telling this report apart from a real race.

## Cause

`std::future::get()` rethrows a stored exception with `std::rethrow_exception`,
which takes a reference to the exception object. The catch block holds that
reference until it ends. The object is freed only when its last reference goes,
and that can be the reference held by the thread that stored the exception,
released afterwards on that thread.

libstdc++ keeps that reference count inside `libstdc++.so`, which is not built
with TSan. TSan therefore never sees the catching thread let go of its reference
before the other thread frees the object, and reports the free as racing with
the earlier read of the exception's message.

There is deliberately **no suppression** for this. TSan matches a suppression
against every frame of both stacks in a report, so one naming these libstdc++
functions would also hide a real race that runs through them, and the pool's
own Task destruction, push rollback and failed-construction paths all do.

## Reproducing it

`exception_refcount_repro.cpp` involves no pool code: a `std::thread` stores a
`std::runtime_error` in a `std::promise`, sleeps (which gives TSan no ordering),
then releases its reference; the main thread catches the exception and reads
`what()`. It is not part of the CMake build, since its job is to make TSan
report. From the repository root, with either compiler:

```sh
clang++ -std=c++20 -O1 -g -fsanitize=thread -Wall -Wextra -Wpedantic -Werror -pthread \
    tools/tsan/exception_refcount_repro.cpp -o /tmp/exception_refcount_repro
/tmp/exception_refcount_repro; echo "exit $?"

g++ -std=c++20 -O1 -g -fsanitize=thread -Wall -Wextra -Wpedantic -Werror -pthread \
    tools/tsan/exception_refcount_repro.cpp -o /tmp/exception_refcount_repro
/tmp/exception_refcount_repro; echo "exit $?"
```

Expected, under both compilers: it prints `rounds=200 wrong_messages=0`, TSan
prints **2** `WARNING: ThreadSanitizer: data race` reports, and the exit status
is **66** (TSan's, for a run with reports). The two reports:

1. Write by the storing thread: `#0 operator delete(void*)`,
   `#1 std::runtime_error::~runtime_error()` (libstdc++.so.6),
   `#2 std::__exception_ptr::exception_ptr::_M_release()` (libstdc++.so.6).
   Previous read by the main thread: `strlen` / `std::char_traits<char>::length`
   / `std::string(const char*)`, from `error.what()` in `main`.
2. Write by the storing thread: `#0 free`,
   `#1 std::__exception_ptr::exception_ptr::_M_release()` (libstdc++.so.6),
   `#2 std::__future_base::_Result<int>::_M_destroy()`.
   Previous read by the main thread: `#0 main`, at the `error.what()` line.

(With g++, the sanitizer's own interceptor frames carry file and line
information and `strlen` appears twice; the frames above are otherwise the same.)

Last confirmed with clang++ 21.1.8 and g++ 15.2.0 (libstdc++ 15), Ubuntu 26.04
under WSL2, 2026-10-16.

## The signature in this project's tests

Seen in `ThreadPoolSubmit.TaskExceptionReachesItsFuture` (clang++ TSan build),
the only test that reads the message of an exception thrown on a worker. Under
ctest it is rare (first seen on 2026-10-16, in one of 20 runs of the suite,
after many earlier clean runs), but `--gtest_repeat=3000` in a single process
shows it reliably. As captured at `eb881fb`:

**Report 1.** Write of size 8 by worker thread T1:

```
#0  operator delete(void*)
#1  std::runtime_error::~runtime_error()                        (libstdc++.so.6)
#2  std::__future_base::_Result<R>::~_Result()
#3  std::__future_base::_Result<R>::_M_destroy()
#4  std::__future_base::_Result_base::_Deleter::operator()(...)
#5  std::unique_ptr<_Result_base, _Result_base::_Deleter>::~unique_ptr()
#6  std::__future_base::_State_baseV2::~_State_baseV2()
#7  std::__future_base::_Task_state_base<R()>::~_Task_state_base()
#8  std::__future_base::_Task_state<LAMBDA, std::allocator<int>, R()>::~_Task_state()
#9  std::destroy_at<_Task_state<...>>(...)
#10 std::allocator_traits<std::allocator<int>>::destroy<_Task_state<...>>(...)
#11 std::_Sp_counted_ptr_inplace<_Task_state<...>, ...>::_M_dispose()
#12 std::_Sp_counted_base<...>::_M_release_last_use()
#13 std::_Sp_counted_base<...>::_M_release()
#14 std::__shared_count<...>::~__shared_count()
#15 std::__shared_ptr<_Task_state_base<R()>, ...>::~__shared_ptr()
#16 std::shared_ptr<_Task_state_base<R()>>::~shared_ptr()
#17 std::packaged_task<R()>::~packaged_task()
#18 tsched::detail::Task::Holder<std::packaged_task<R()>>::~Holder()   task.hpp:47
#19 tsched::detail::Task::Holder<std::packaged_task<R()>>::~Holder()   task.hpp:47
#20 std::default_delete<tsched::detail::Task::Callable>::operator()(...)
#21 std::unique_ptr<tsched::detail::Task::Callable>::~unique_ptr()
#22 tsched::detail::Task::run() &&                                     task.hpp:38
#23 tsched::ThreadPool::worker_loop(std::stop_token, unsigned long)    thread_pool.cpp:175
```

Previous read of size 5 by the main (test) thread:

```
#0 strlen
#1 std::char_traits<char>::length(char const*)
#2 std::string::basic_string(char const*, ...)
#3 ...TaskExceptionReachesItsFuture_Test::TestBody()   thread_pool_submit_test.cpp:275 (error.what())
```

**Report 2.** Write of size 8 by worker thread T1: the same stack, except
`#0 free` and `#1 std::__exception_ptr::exception_ptr::_M_release()`
(libstdc++.so.6). Previous read of size 8 by the main thread:
`#0 ...TaskExceptionReachesItsFuture_Test::TestBody()`, at the same line.

`R` is the task's result type (`int` here) and `LAMBDA` the test's task.

## The rule

A TSan failure may be dismissed as this false positive only if **both** hold:

1. **Both stacks of every report match the signature above, frame for frame:**
   the same functions in the same order on the write side, and on the read side
   a test body reading the caught exception. Only these may differ: file and
   line numbers (they move with edits), addresses and build IDs, the result type
   `R`, the task's lambda type, and the name and line of the test body doing the
   read.
2. **The minimal reproduction still reproduces it on the current toolchain:**
   built and run as above, it gives 2 reports with the `#1` frames listed above,
   and exit status 66.

Anything else, including a report that matches only partly, is investigated as
a real race.
