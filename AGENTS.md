# AGENTS.md — Multi-Threaded Task Scheduler

Rules for the coding agent (Claude Code) working on this project.

## Workflow
- Propose an approach before writing code. Wait for explicit approval before implementing.
- Build one feature slice at a time (see SPEC.md's numbered slices). Do not start the next slice until the current one is confirmed working.
- Never commit without the owner explicitly saying "confirmed working" (or equivalent) after reviewing the change.
- If a design decision in the spec is marked as an open question, stop and ask rather than assuming an answer. (Note: testing framework, C++ standard, queue design, and shutdown semantics are now locked in SPEC.md — only the benchmark workload choice remains genuinely open.)

## Code conventions
- C++20, CMake build
- Every concurrency primitive change needs a test that actually exercises concurrent access, not just a single-threaded call — a thread pool that "compiles and runs once" proves nothing about correctness
- Treat data races and undefined behavior as the actual risk in this codebase, not a formality — reach for ThreadSanitizer during development on anything touching shared state, and call it out explicitly if it's not available in the environment
- Per-thread work-stealing queues are locked-in v1 scope, not a premature optimization — build the simple single-thread-queue mental model first within slice 1/2 if that helps get correctness right, but the shipped v1 design is work-stealing, not a single shared queue
- No other premature optimization (e.g. lock-free structures) before the work-stealing version exists and is benchmarked — that stays a genuine v2 stretch goal

## Session handoff
- Maintain a CLAUDE_SUMMARY.md changelog (gitignored) documenting what's been built each session, matching the pattern used across the owner's other projects
