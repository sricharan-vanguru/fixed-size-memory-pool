# Fixed-Size Memory Pool Allocator

A modular C++20 memory-pool library. Its small fixed-capacity core pre-allocates
one contiguous region, while an optional chunk layer adds stable growth and
reclamation. It is a focused study of manual memory management, alignment,
object lifetime, cache behavior, policies, and provider-based storage.

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
  memory_provider.hpp                backing-storage provider contract
  new_delete_memory_provider.hpp     default provider
  memory_chunk.hpp                   one stable block-storage allocation
  chunk_manager.hpp                  growth, lookup, and reclamation
  chunk_policies.hpp                 growth and reclamation configuration
  exhaustion_policies.hpp            compile-time exhaustion strategies
  chunk_pool.hpp                     policy-based growing pool facade
  size_class_selector.hpp            size/alignment routing rules
  segregated_allocator_options.hpp   multi-size configuration
  segregated_allocator_statistics.hpp per-class statistics
  segregated_allocator.hpp           mixed-size allocator facade
  pool_memory_resource.hpp            standard PMR adapter

src/                                 compiled implementation
  chunk_manager.cpp                  multi-chunk ownership and growth
  fixed_size_memory_pool.cpp         storage and free-list coordination
  memory_chunk.cpp                   per-chunk block allocation
  new_delete_memory_provider.cpp     new/delete backing storage
  size_class_selector.cpp            smallest-class selection
  segregated_allocator.cpp           allocation/deallocation routing
  pool_memory_resource.cpp            PMR and upstream adaptation
  detail/block_layout.*              alignment and overflow-safe layout
  detail/diagnostic_state.*          block state and integrity checks
  detail/memory_guard.*              poisoning and canaries
  detail/statistics_tracker.*        optional counter updates

tests/
  fixed_size_memory_pool_tests.cpp   core behavior and object lifetime
  diagnostics_tests.cpp              misuse and corruption detection
  object_pool_tests.cpp              typed construction and RAII ownership
  phase3_tests.cpp                   providers, chunks, growth, and policies
  segregated_allocator_tests.cpp     mixed-size routing and fallback
  pmr_tests.cpp                      standard-container integration
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
./build/growing_pool_example
./build/segregated_allocator_example
./build/pmr_example
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
| `MemoryChunk::allocate()` | O(1) | O(1) |
| `ChunkManager::try_allocate()` | O(number of chunks) | O(1) |
| `ChunkManager::grow()` | O(blocks in new chunk) | O(new chunk size) |
| `SizeClassSelector::select()` | O(log(number of classes)) | O(1) |
| `SegregatedAllocator::allocate()` | O(log(classes) + chunks in selected class) | O(1) normally |
| `SegregatedAllocator::deallocate()` | O(total chunks across classes) | O(1) |
| `PoolMemoryResource::allocate()` | Same as `SegregatedAllocator::allocate()` | O(1) normally |

## Limitations

- It is not thread-safe by design.
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
