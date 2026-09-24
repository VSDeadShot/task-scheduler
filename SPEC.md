# Multi-Threaded Task Scheduler / Thread Pool — v1 Spec

## Problem
A from-scratch C++ thread pool / task scheduler library, built to demonstrate real systems-level understanding (concurrency, synchronization, memory management).

## Goals (v1)
- A reusable thread pool: fixed number of worker threads
- Per-thread work-stealing queues (not a single shared queue) — each worker owns a local deque; idle workers steal from the back of another thread's queue when their own is empty
- `submit(task)` API that returns a `std::future` so callers can retrieve results/exceptions
- Thread-safe work-stealing queue (per-deque lock). The owning worker pushes to the back and pops from the front, so each queue is FIFO; idle workers steal from the back. FIFO is chosen for the workload: Slice 3 runs independent `submit()`/`std::future` tasks, and FIFO keeps early submissions from being starved by later ones. A v2 lock-free Chase-Lev deque is inherently LIFO for its owner and would revisit this ordering
- Graceful shutdown: `stop()`/destruction drains all currently-queued work before threads exit, using `std::jthread` + `std::stop_token` for cooperative cancellation signaling
- Correctness proven under real concurrent stress, not just single-threaded tests
- A benchmark harness comparing single-threaded vs pooled execution throughput, and (since work-stealing is now v1 scope) pooled-with-stealing vs pooled-without-stealing under uneven task distribution

## Non-goals (v1)
- No priority queue / task priorities (v2 stretch goal — v1 is FIFO within a given thread's queue: the owner pops tasks in the order they were pushed)
- No distributed/multi-process scheduling — single-process only
- No immediate/cancel-pending shutdown mode in v1 — `std::stop_token` already gives a natural hook for this later, but graceful drain is the only shutdown path built and tested in v1

## Locked stack
- **Language standard:** C++20 — `std::jthread` for automatic join-on-destruction and built-in `std::stop_token` support, used for both thread lifecycle and cooperative shutdown signaling
- **Build system:** CMake
- **Testing:** GoogleTest (+ gmock if mocking is ever needed)
- **Benchmarking:** Google Benchmark, or a hand-rolled timing harness if pulling in another dependency feels like overkill — still open, lower priority than the four below

## Resolved design decisions
1. **Testing framework** — GoogleTest. More widely used in industry codebases than Catch2.
2. **C++ standard** — C++20. Confirm the dev machine's toolchain (compiler version) supports it cleanly before slice 1 starts.
3. **Queue design** — per-thread work-stealing queues. This is the real systems-engineering story (cache locality, contention reduction, load balancing) versus a single global queue, and makes explicit the kind of design trade-off a design review would scrutinize.
4. **Shutdown semantics** — graceful drain by default via `stop_token`, no separate cancel-now mode in v1.
5. **Benchmark workload — proposed, not yet confirmed:** a Mandelbrot-set tile renderer, split into many small independent tiles. CPU-bound, embarrassingly parallel, shows throughput differences clearly, and produces a visual/screenshot-able output that doubles as a nice demo artifact. Alternative considered: chunked matrix multiplication (also works, less visually interesting). Confirm before slice 5, or come back to this later — not blocking slices 1-4.

## v1 feature slices (build one at a time, confirm working before moving to the next)
1. Core thread pool skeleton: fixed worker threads, each with its own work-stealing deque, start/stop lifecycle — no task submission yet, just prove threads launch and join cleanly
2. Thread-safe work-stealing queue (per-thread deque, steal-from-back), with unit tests specifically targeting race conditions and steal correctness (not just happy-path push/pop)
3. `submit()` returning `std::future`, wired to the queue and worker loop, including the steal path when a worker's own queue is empty
4. Graceful shutdown via `stop_token` (drain mode only), with tests proving no task is silently dropped
5. Benchmark harness + the confirmed workload, single-threaded vs pooled vs pooled-with-stealing-under-uneven-load comparison
6. Stress test: high task volume, verify no deadlocks/races via repeated runs (consider running under ThreadSanitizer)
