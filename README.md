# Fixed-Size Memory Pool Allocator

A compact C++20 memory pool that pre-allocates one contiguous region and serves
fixed-size allocations in constant time. It is intended as a focused study of
manual memory management, alignment, object lifetime, and cache behavior.

## What it demonstrates

- O(1) allocation and deallocation through an intrusive free list
- No external fragmentation because every block has the same size
- Configurable power-of-two alignment, including cache-line alignment
- Placement construction and explicit destruction through `create<T>` and
  `destroy<T>`
- Type-safe `ObjectPool<T>` construction with automatic size and alignment
- Move-only `PoolPtr<T>` ownership for automatic pooled-object destruction
- Exception-safe object construction: a throwing constructor returns its block
  to the pool
- Ownership and block-boundary validation on deallocation
- Optional block-state tracking and deterministic double-free detection
- Optional memory poisoning, guard canaries, integrity validation, and leak callback
- Optional allocation, peak-use, failure, and reserved-memory statistics
- Copy and move disabled so the backing allocation cannot be invalidated

The base implementation is intentionally not synchronized. A caller can give
each thread its own pool for the lowest overhead or place a lock around a shared
pool. Adding a mutex inside this class would hide that policy choice and change
the latency characteristics being studied.

## Design

The free blocks themselves store the free-list links, so the allocator needs no
per-allocation metadata. Allocation removes the list head; deallocation adds a
block back to the head. Both operations touch a constant number of pointers.

```text
contiguous backing allocation

+----------+    +----------+    +----------+    +----------+
| free     | -> | free     | -> | in use   |    | free     | -> null
+----------+    +----------+    +----------+    +----------+
```

### Architecture

The project uses a compiled allocator core with small public headers. Template
object-lifetime helpers remain in the public header, while layout calculations,
state tracking, canaries, poisoning, and statistics collection are private
implementation modules.

```text
include/memory_pool/                 public API
  fixed_size_memory_pool.hpp         allocator interface and templates
  pool_options.hpp                   optional behavior configuration
  pool_statistics.hpp                statistics result type
  pool_errors.hpp                    allocator-specific exceptions
  object_pool.hpp                    type-safe facade for one object type
  pool_ptr.hpp                       RAII pointer and pool-aware deleter

src/                                 compiled implementation
  fixed_size_memory_pool.cpp         storage and free-list coordination
  detail/block_layout.*              alignment and overflow-safe layout
  detail/diagnostic_state.*          block state and integrity checks
  detail/memory_guard.*              poisoning and canaries
  detail/statistics_tracker.*        optional counter updates

tests/
  fixed_size_memory_pool_tests.cpp   core behavior and object lifetime
  diagnostics_tests.cpp              misuse and corruption detection
  object_pool_tests.cpp              typed construction and RAII ownership
  statistics_tests.cpp               counter behavior
```

Internal modules are not part of the supported public API. Applications should
include headers only from `include/memory_pool` and link the `memory_pool` CMake
target.

### Fragmentation

The pool cannot suffer external fragmentation: every free block can satisfy
every allocation request supported by the pool. Internal fragmentation is the
trade-off. An object smaller than a block leaves unused bytes inside that block.
Pools are therefore most effective when grouped by object size.

### Cache locality

All blocks live in one contiguous allocation, improving spatial locality and
reducing allocator metadata traffic compared with unrelated heap allocations.
The LIFO free list also tends to reuse recently touched cache lines. For shared
cross-thread pools, that same behavior can increase cache-line bouncing; a
per-thread pool or batched transfer of free blocks is often better.

### Alignment

The requested alignment must be a power of two and at least pointer-aligned.
Block size is rounded up to that alignment so every block begins at a valid
address. Over-aligned objects are supported when the pool is constructed with a
sufficient alignment. Using 64-byte blocks does not automatically prevent false
sharing if different threads mutate adjacent blocks; padding or ownership
partitioning may still be needed.

### Optional diagnostics

The default options preserve the original compact behavior and allocate no
per-block state metadata. Diagnostics can be enabled explicitly when developing
or testing a caller:

```cpp
memory_pool::PoolOptions options{
    .diagnostics = memory_pool::DiagnosticMode::enabled,
    .poison_memory = true,
    .guard_bytes = true,
    .collect_statistics = true,
};

memory_pool::FixedSizeMemoryPool pool(
    64, 1024, alignof(std::max_align_t), options);
```

Diagnostic mode tracks whether each block is free or allocated, which makes a
double-free deterministic:

```cpp
void* block = pool.allocate();
pool.deallocate(block);
pool.deallocate(block); // throws memory_pool::DoubleFreeError
```

`validate_integrity()` performs an O(n) debug traversal that verifies free-list
addresses, uniqueness, state, and the available-block count. `debug_dump()`
prints block states to an output stream. These operations intentionally require
diagnostic mode and are not part of the constant-time allocation path.

Memory poisoning writes `0xCD` into allocated payloads and `0xDD` into released
payloads. Guard mode reserves canaries immediately before and after each user
payload and verifies them during deallocation. A changed canary throws
`MemoryCorruptionError`, typically indicating an underflow or overflow.

A non-throwing `leak_handler` callback may be supplied in `PoolOptions`. If
diagnostics are enabled and blocks remain live when the pool is destroyed, the
callback receives the outstanding block count. The callback must not access the
pool being destroyed.

Statistics are available through `statistics()`:

```cpp
const auto& statistics = pool.statistics();
std::cout << statistics.peak_allocated << '\n';
std::cout << statistics.failed_allocations << '\n';
```

All diagnostics are optional. Poisoning, guards, state tracking, statistics, and
callbacks add storage or execution overhead and should be selected according to
the workload.

### Type-safe objects and RAII

`ObjectPool<T>` derives the correct block size and alignment from `T`, so callers
do not need to configure those values manually. Constructor arguments are
perfectly forwarded:

```cpp
struct Session {
    int id;
    std::string state;
};

memory_pool::ObjectPool<Session> sessions(128);
Session* session = sessions.create(42, "active");
sessions.destroy(session);
```

Prefer `make_unique()` when ownership stays within one scope. It returns a
move-only `PoolPtr<T>` that destroys the object and returns its block when the
pointer is reset, leaves scope, or participates in exception unwinding:

```cpp
memory_pool::ObjectPool<Session> sessions(128);
auto session = sessions.make_unique(42, "active");
```

`PoolPtr<T>` stores a non-owning reference to its originating pool. The pool
must therefore outlive every `PoolPtr` and raw pointer created from it. This is
the same ordering naturally provided by declaring the pool before its pointers.
The pool is intentionally non-copyable and non-movable, and object creation from
a temporary `ObjectPool` is rejected at compile time. Runtime lifetime tracking
is not added because it would require a shared control block and reference-count
overhead on this low-level path.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run the example and microbenchmark:

```bash
./build/memory_pool_example
./build/object_pool_example
./build/memory_pool_benchmark
```

Enable AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake -S . -B build-sanitize \
  -DMEMORY_POOL_ENABLE_SANITIZERS=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

Benchmark results depend on the compiler, optimization level, standard library,
CPU, and system load. The included benchmark is a reproducible comparison aid,
not a universal performance claim.

## Complexity

| Operation | Time | Additional space |
|---|---:|---:|
| Construct pool | O(n) | O(n) backing allocation |
| `allocate()` | O(1) | O(1) |
| `deallocate()` | O(1) | O(1) |
| `create<T>()` | O(1) plus `T` construction | O(1) |
| `destroy<T>()` | O(1) plus `T` destruction | O(1) |
| `ObjectPool<T>::make_unique()` | O(1) plus `T` construction | O(1) |

## Limitations

- It is not thread-safe by design.
- Double-free detection requires explicitly enabled diagnostic mode.
- All live objects must be destroyed before the pool itself is destroyed.
- Every `PoolPtr<T>` must be destroyed before its originating pool.
- The pool is fixed-capacity and never grows.
