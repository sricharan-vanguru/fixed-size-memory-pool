# Versioning and Compatibility

The project uses semantic versioning for tagged releases:

- **Major:** incompatible changes to installed public headers, documented
  behavior, or the exported `memory_pool::memory_pool` target.
- **Minor:** backward-compatible allocator features and public API additions.
- **Patch:** backward-compatible fixes, tests, documentation, and build updates.

Only installed headers under `include/memory_pool` form the supported C++ API.
Files under `src` and `src/detail`, test helpers, examples, benchmarks, and
undocumented implementation details may change without an API-compatibility
guarantee.

The current source reports version 1.7.0 but has not yet been tagged. This policy
applies when the first release tag is created; Git history remains the source of
truth for all earlier development milestones.

Conan and vcpkg packaging are intentionally deferred until the first tagged API
has been consumed successfully through the CMake package. Adding registry
metadata before that point would create another compatibility surface without
providing additional allocator validation.
