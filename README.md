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
- Exception-safe object construction: a throwing constructor returns its block
  to the pool
- Ownership and block-boundary validation on deallocation
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

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run the example and microbenchmark:

```bash
./build/memory_pool_example
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

## Limitations

- It is not thread-safe by design.
- Double-free detection is not provided in the release-oriented fast path.
- All live objects must be destroyed before the pool itself is destroyed.
- The pool is fixed-capacity and never grows.

