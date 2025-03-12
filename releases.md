# Releases

## v1.10.0

  - `IO::Event::Profiler` is moved to dedicated gem: [fiber-profiler](https://github.com/socketry/fiber-profiler).
  - Perform runtime checks for native selectors to ensure they are supported in the current environment. While compile-time checks determine availability, restrictions like seccomp and SELinux may still prevent them from working.

## v1.9.0

  - Improved `IO::Event::Profiler` for detecting stalls.

## v1.8.0

  - Detecting fibers that are stalling the event loop.

## v1.7.5

  - Fix `process_wait` race condition on EPoll that could cause a hang.
