# Changelog

This project has not published a tagged release yet. The current CMake project
version is 1.7.0; the entries below describe genuine development milestones.

## Unreleased

### Added

- Install and export rules for `find_package(fixed_size_memory_pool)`.
- Exported CMake target `memory_pool::memory_pool`.
- External installed-package consumer test.
- Formatting, optional clang-tidy, and warnings-as-errors configuration.
- Architecture and API/lifetime documentation.

### Changed

- Examples and benchmarks can be disabled independently in CMake.
- CI covers GCC and Clang in Debug and Release configurations, sanitizers,
  formatting, warnings-as-errors, and installed-package consumption.

## Development milestones

- Phase 7: monotonic lifetime arena with aligned allocation, bulk reset,
  reusable chunks, typed destruction, statistics, tests, and benchmark.
- Phase 6: synchronized and thread-cached allocators with cross-thread frees.
- Phase 5: standard PMR memory-resource integration.
- Phase 4: segregated size classes and exact provider fallback.
- Phase 3: provider abstraction, stable chunks, growth, and reclamation.
- Phase 2: typed object pools and move-only pooled ownership.
- Phase 1: optional diagnostics, guards, poisoning, statistics, and modular core.
