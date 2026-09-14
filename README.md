# Fixed-Size Memory Pool Allocator

A modular C++20 memory-pool library. Its small fixed-capacity core pre-allocates
one contiguous region, while an optional chunk layer adds stable growth and
reclamation. It is a focused study of manual memory management, alignment,
object lifetime, cache behavior, policies, and provider-based storage.

Detailed references: [architecture](docs/ARCHITECTURE.md),
[API and lifetime guide](docs/API_GUIDE.md),
[versioning policy](docs/VERSIONING.md), and [changelog](CHANGELOG.md).

## What it demonstrates

- O(1) allocation and deallocation through an intrusive free list
- No external fragmentation because every block has the same size
- Configurable power-of-two alignment, including cache-line alignment
- Placement construction and explicit destruction through `create<T>` and
  `destroy<T>`
- Type-safe `ObjectPool<T>` construction with automatic size and alignment
- Move-only `PoolPtr<T>` ownership for automatic pooled-object destruction
- Stable multi-chunk storage with fixed or geometric growth
- Return-null, throw, grow, and heap-fallback exhaustion policies
- Empty-chunk reclamation with configurable spare capacity
- Injectable backing-memory providers
- Configurable multi-size classes from 8 through 4096 bytes by default
- Aligned system fallback for large and unsupported over-aligned requests
- Per-class usage and internal-fragmentation statistics
- `std::pmr::memory_resource` integration for standard-library containers
- Configurable upstream PMR resource with identity-based equality
- Lock-policy-based synchronized wrapper for shared allocation
- Per-thread size-class caches with configurable refill and flush watermarks
- Safe cross-thread deallocation through synchronized central storage
- Thread-safe cache, live-allocation, and remote-free statistics
- Aligned monotonic allocation for request, frame, and compiler-pass lifetimes
- Bulk arena reset with reusable or releasable growth chunks
- Reverse-order destruction for non-trivial arena-created objects
- Exception-safe object construction: a throwing constructor returns its block
  to the pool
- Ownership and block-boundary validation on deallocation
- Optional block-state tracking and deterministic double-free detection
- Optional memory poisoning, guard canaries, integrity validation, and leak callback
- Optional allocation, peak-use, failure, and reserved-memory statistics
- Copy and move disabled so the backing allocation cannot be invalidated

The base implementation remains intentionally unsynchronized. Applications can
use one pool per thread, select `SynchronizedAllocator` for a simple shared
baseline, or use `ThreadCachedAllocator` to reduce central allocator traffic.
Keeping concurrency in separate wrappers preserves the cost model and single
responsibility of the original allocator.

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

Internal modules are not part of the supported public API. Applications should
include headers only from `include/memory_pool` and link the
`memory_pool::memory_pool` CMake target.

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

### Storage providers and growing chunks

`ChunkManager` separates fast block reuse from backing-memory acquisition. Each
`MemoryChunk` owns one provider allocation and never moves, so adding another
chunk does not invalidate existing pointers. `try_allocate()` examines existing
chunks only; the provider is called only by growth, reclamation, destruction, or
heap fallback.

```cpp
memory_pool::GrowingChunkPool pool({
    .block_size = 64,
    .initial_blocks = 32,
    .alignment = 64,
    .growth = memory_pool::GrowthPolicy::geometric(2, 256),
    .reclamation = {.spare_empty_chunks = 1},
});

void* block = pool.allocate();
pool.deallocate(block);
```

This configuration creates 32, 64, 128, then 256-block chunks and caps later
chunks at 256 blocks. When more than one chunk is completely free, reclamation
releases the extras while retaining one spare.

Four aliases select exhaustion behavior at compile time:

| Alias | Behavior when current chunks are full |
|---|---|
| `NullableChunkPool` | Return `nullptr` |
| `ThrowingChunkPool` | Throw `std::bad_alloc` |
| `GrowingChunkPool` | Acquire the next configured chunk |
| `HeapFallbackChunkPool` | Allocate one fallback block from the provider |

`IMemoryProvider` is the Strategy boundary for backing storage. `allocate` must
return a non-null aligned allocation or throw, and `deallocate` must not throw.
The manager and its chunks share ownership of the provider, so it remains alive
until all storage has been released. The default `NewDeleteMemoryProvider` uses
ordinary or aligned `new`/`delete` as appropriate. Provider calls deliberately
remain outside normal block reuse.

`MemoryChunk` validates block boundaries and allocation state, making foreign
pointers, interior pointers, and duplicate returns deterministic errors.

### Multi-size segregated allocation

`SegregatedAllocator` owns one growing chunk pool per size class. The default
classes are 8, 16, 32, 64, 128, 256, 512, 1024, 2048, and 4096 bytes. A request
is routed to the smallest class that satisfies both its size and alignment:

```cpp
memory_pool::SegregatedAllocator allocator;

void* first = allocator.allocate(24, 8);   // 32-byte class
void* second = allocator.allocate(24, 64); // 64-byte class

allocator.deallocate(first, 24, 8);
allocator.deallocate(second, 24, 64);
```

A zero-byte request is normalized to one byte. Alignments must be non-zero
powers of two. A request larger than the maximum class, or with an alignment
that no configured class supports, is allocated directly through the memory
provider with the requested alignment.

Pooled allocations do not carry an allocation header. Unsized `deallocate`
finds the owning size class through chunk metadata. The sized overload checks
that the supplied size and alignment select the same class and throws
`AllocationMismatchError` when they do not. Since multiple sizes can share one
class, it detects routing mistakes rather than exact byte differences within
that class. Fallback allocations keep exact size/alignment metadata and require
an exact match during sized deallocation.

Statistics are cumulative and available through `statistics()`. Each class
reports requests, successful allocations, current/peak use, requested bytes,
served bytes, and internal-fragmentation bytes. Internal fragmentation is the
difference between the selected class size and the normalized requested size.

### Standard-library PMR integration

`PoolMemoryResource` adapts `SegregatedAllocator` to
`std::pmr::memory_resource`, allowing standard PMR containers to use the
multi-size pool without a container-specific allocator:

```cpp
memory_pool::PoolMemoryResource resource;

{
    std::pmr::vector<int> values(&resource);
    std::pmr::string text(&resource);
    std::pmr::list<int> nodes(&resource);
    std::pmr::unordered_map<int, int> lookup(&resource);
}
```

Declaration order is part of the ownership contract. Every container and
`std::pmr::polymorphic_allocator` using the resource must be destroyed before
the `PoolMemoryResource`. When a custom upstream resource is supplied, that
upstream is non-owning and must remain alive until the pool resource has been
destroyed. The default upstream is `std::pmr::get_default_resource()`.

Two `PoolMemoryResource` objects compare unequal even when configured
identically because each owns different allocation state. A resource compares
equal only to itself. This prevents a container from returning memory to a
different pool. Requests above the largest class and unsupported over-aligned
requests are forwarded to the configured upstream with their exact size and
alignment.

| Resource | Synchronization | Size classes | Diagnostics/statistics |
|---|---|---|---|
| `PoolMemoryResource` | None | Explicitly configurable | Per-class and fallback statistics |
| `std::pmr::unsynchronized_pool_resource` | None | Implementation managed | Standard resource interface only |
| `std::pmr::synchronized_pool_resource` | Internal synchronization | Implementation managed | Standard resource interface only |

The comparison is behavioral, not a universal performance ranking. Workload,
standard-library implementation, compiler, and configuration determine which
resource is faster.

A separate `PoolAllocator<T>` is intentionally not provided. The standard
`std::pmr::polymorphic_allocator<T>` already supplies typed `allocate(n)`,
container integration, rebinding through PMR construction rules, and equality
based on the underlying resource. Another adapter would duplicate that behavior
without improving the ownership model. Node-based containers are natural pool
users; large contiguous growth requests may use larger classes or upstream
fallback and should be benchmarked for the actual workload.

### Concurrency layers

`SynchronizedAllocator` is the correctness-first shared allocator. It composes
one `SegregatedAllocator` with a mutex and protects allocation, deallocation,
ownership queries, and statistics snapshots with that same lock:

```cpp
memory_pool::SynchronizedAllocator allocator;

void* block = allocator.allocate(48, 16);
allocator.deallocate(block, 48, 16);
```

The wrapper is an alias of `BasicSynchronizedAllocator<std::mutex>`. A custom
BasicLockable type can be supplied when a workload needs a different lock
policy. The contained allocator remains private so callers cannot bypass the
lock accidentally.

`ThreadCachedAllocator` adds one small-block cache per thread and size class:

```cpp
memory_pool::ThreadCachedAllocator allocator(
    {},
    {.low_watermark = 8, .high_watermark = 32, .refill_batch = 16});
```

An empty local cache obtains a batch from synchronized central storage. A
same-thread deallocation enters that thread's cache. When a bin grows above the
high watermark, a batch is returned until the low watermark is reached. Calling
`release_current_thread_cache()` returns all cached blocks owned by the caller;
thread exit performs the same cleanup automatically.

Every live allocation records a stable thread identifier. If another thread
returns it, the block bypasses that thread's cache and goes directly to central
storage. Allocation records are split across 64 lock shards, while the
underlying `SegregatedAllocator` has a separate central mutex. This makes
cross-thread frees safe without letting one thread access another thread's
local vectors. Cached blocks remain allocated from the central allocator until
they are flushed, so the cache statistics distinguish logical live allocations,
cached blocks, and central operations.

`statistics()` reads race-free atomic counters. During active allocation, its
fields are an approximate observation rather than one linearizable instant, so
temporary cross-field relationships should not be treated as invariants.
`central_statistics()` locks central storage and returns a consistent snapshot
of the underlying allocator itself.

All worker threads must finish before `ThreadCachedAllocator` is destroyed.
Concurrent destruction and allocator calls are invalid. The implementation is
mutex-based, not lock-free; no ABA or memory-reclamation claim is made.

The included concurrency benchmark compares immediate shared allocate/free
pairs. One local 8-thread Release run measured 566.17 ns/pair for the
synchronized baseline and 247.55 ns/pair for the thread-cached allocator. This
demonstrates the intended batching effect for that run only; other machines and
workloads will differ.

### Lifetime-based monotonic arena

`MonotonicArena` serves workloads whose allocations share one lifetime, such as
an HTTP request, one compiler pass, or one rendered frame. Each allocation moves
an aligned cursor forward. There is deliberately no individual `deallocate`;
`reset()` invalidates every arena pointer together and makes the storage
available for the next lifetime.

```cpp
memory_pool::MonotonicArena arena;

SyntaxNode* left = arena.create<SyntaxNode>("4");
SyntaxNode* right = arena.create<SyntaxNode>("5");
SyntaxNode* root = arena.create<SyntaxNode>("+", left, right);

// Use the complete tree, then finish this compiler pass.
arena.reset();
```

`create<T>()` constructs an object in arena storage. Non-trivial objects are
registered and destroyed in reverse construction order during `reset()` or
arena destruction; arena-managed destructors must be `noexcept`. Raw
`allocate()` only reserves bytes and never manages object destruction, so a
caller using placement construction on raw storage remains responsible for
ending that object's lifetime before reset.

The default geometric policy grows chunks by a factor of two up to 1 MiB.
Requests larger than that cap receive a fitting dedicated chunk. Fixed growth
is also available. `retain_all_chunks` rewinds every acquired chunk at reset for
stable repeated workloads, while `retain_initial_chunk` releases growth chunks
to reduce retained memory. Existing addresses never move when the arena grows.

Construction failure rewinds its storage when it is still the latest arena
allocation. If a constructor recursively allocates from the same arena before
throwing, that monotonic space remains reserved until reset, without corrupting
earlier objects. Statistics expose cumulative requests, failures, padding,
construction/destruction activity, resets, and current/peak storage.

The arena is unsynchronized and should be confined to one thread or protected
externally. `owns(pointer)` reports whether an address lies in currently
retained backing storage; it does not prove that an object at that address is
still alive.

The arena benchmark compares repeated 32-byte aligned allocations and bulk
resets against `std::pmr::monotonic_buffer_resource` using equally sized reused
storage. Three local GCC 13.3 Release runs measured 9.15-11.08 ns/allocation
for this instrumented arena and 2.36-2.75 ns/allocation for the standard PMR
resource. The result is a transparent baseline, not a claim that the custom
arena is faster.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Warnings can be promoted to errors with
`-DMEMORY_POOL_WARNINGS_AS_ERRORS=ON`. Examples and benchmarks can be omitted
with `-DMEMORY_POOL_BUILD_EXAMPLES=OFF` and
`-DMEMORY_POOL_BUILD_BENCHMARKS=OFF`. When `clang-tidy` is installed, enable it
with `-DMEMORY_POOL_ENABLE_CLANG_TIDY=ON`.

Check repository formatting with:

```bash
clang-format --dry-run --Werror \
  $(find include src tests examples benchmarks -type f \
    \( -name '*.hpp' -o -name '*.cpp' \))
```

## Install and consume

Install the library and its CMake package files:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DMEMORY_POOL_BUILD_EXAMPLES=OFF \
  -DMEMORY_POOL_BUILD_BENCHMARKS=OFF
cmake --build build --parallel
cmake --install build --prefix /path/to/install
```

Consume it from another CMake project:

```cmake
find_package(fixed_size_memory_pool 1.7 REQUIRED)
target_link_libraries(my_target PRIVATE memory_pool::memory_pool)
```

Pass the installation prefix through `CMAKE_PREFIX_PATH` when it is outside a
standard system location. The package requires C++20 and finds its Threads
dependency automatically.

Run the example and microbenchmark:

```bash
./build/memory_pool_example
./build/object_pool_example
./build/growing_pool_example
./build/segregated_allocator_example
./build/pmr_example
./build/concurrency_example
./build/arena_example
./build/memory_pool_benchmark
./build/concurrency_benchmark
./build/arena_benchmark
```

Enable AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake -S . -B build-sanitize \
  -DMEMORY_POOL_ENABLE_SANITIZERS=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

Run ThreadSanitizer in a separate build because it cannot be combined with
AddressSanitizer:

```bash
cmake -S . -B build-tsan \
  -DMEMORY_POOL_ENABLE_THREAD_SANITIZER=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure
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
| `MemoryChunk::allocate()` | O(1) | O(1) |
| `ChunkManager::try_allocate()` | O(number of chunks) | O(1) |
| `ChunkManager::grow()` | O(blocks in new chunk) | O(new chunk size) |
| `SizeClassSelector::select()` | O(log(number of classes)) | O(1) |
| `SegregatedAllocator::allocate()` | O(log(classes) + chunks in selected class) | O(1) normally |
| `SegregatedAllocator::deallocate()` | O(total chunks across classes) | O(1) |
| `PoolMemoryResource::allocate()` | Same as `SegregatedAllocator::allocate()` | O(1) normally |
| `SynchronizedAllocator` operation | Underlying operation plus one lock | O(1) |
| Thread-cache hit/local free | O(1) expected, including one metadata-shard lock | O(1) |
| Thread-cache refill/flush | O(batch size) | O(batch size) per local cache |
| `MonotonicArena::allocate()` | O(retained chunks examined), O(1) in the current chunk | O(1), excluding growth |
| `MonotonicArena::create<T>()` | Allocation plus construction; destructor registration is amortized O(1) | Amortized O(1) metadata for non-trivial `T` |
| `MonotonicArena::reset()` | O(chunks + registered destructors) | O(1) |

## Limitations

- The base pools, `SegregatedAllocator`, and `PoolMemoryResource` remain
  unsynchronized by design; use an explicit concurrency wrapper for shared use.
- `FixedSizeMemoryPool` double-free detection requires diagnostic mode;
  `MemoryChunk` always tracks allocation state.
- All live objects must be destroyed before the pool itself is destroyed.
- Every `PoolPtr<T>` must be destroyed before its originating pool.
- `FixedSizeMemoryPool` and `ObjectPool<T>` are fixed-capacity and never grow.
- Chunk-based pools preserve pointer stability but currently scan chunks when
  selecting available storage or finding a returned pointer.
- Segregated pooled allocations avoid headers, so unsized deallocation scans
  size classes and chunk ranges to find the owner.
- `PoolMemoryResource` is unsynchronized and must not be shared across threads
  without an external synchronization layer.
- Thread-cache metadata improves diagnostics and remote-free routing but adds a
  sharded lookup to every cached allocation and deallocation.
- Thread-local caches can temporarily retain free blocks until a watermark
  flush, explicit release, or thread exit.
- `MonotonicArena` has no individual free operation, is unsynchronized, and
  invalidates all of its pointers on reset.
- Arena raw allocations do not register destructors; use `create<T>()` for
  automatic reverse-order destruction of non-trivial objects.
