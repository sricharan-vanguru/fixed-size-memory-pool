# API and Lifetime Guide

## Choosing an allocator

| Requirement | Recommended type |
|---|---|
| One fixed object size and capacity | `FixedSizeMemoryPool` or `ObjectPool<T>` |
| One block size with stable growth | `GrowingChunkPool` |
| Mixed allocation sizes | `SegregatedAllocator` |
| Standard PMR containers | `PoolMemoryResource` |
| Simple shared access | `SynchronizedAllocator` |
| Shared small allocations with batching | `ThreadCachedAllocator` |
| Many objects released together | `MonotonicArena` |

## Installed-package usage

```cmake
find_package(fixed_size_memory_pool 1.7 REQUIRED)
target_link_libraries(my_target PRIVATE memory_pool::memory_pool)
```

All APIs require C++20.

## Fixed typed objects

```cpp
#include <memory_pool/object_pool.hpp>

memory_pool::ObjectPool<Session> sessions(128);
auto session = sessions.make_unique(42, "active");
```

`ObjectPool<T>` derives block size and alignment from `T`. `PoolPtr<T>` calls
the destructor and returns the block automatically. Declare the pool before its
pointers so it is destroyed after them.

## Mixed-size allocation

```cpp
#include <memory_pool/segregated_allocator.hpp>

memory_pool::SegregatedAllocator allocator;
void* memory = allocator.allocate(24, 8);  // default 32-byte class
allocator.deallocate(memory, 24, 8);
```

Sized deallocation validates that the supplied values select the original size
class. For provider fallback, size and alignment must match exactly. Unsized
deallocation discovers the owning chunk but may scan size classes.

## PMR containers

```cpp
#include <memory_pool/pool_memory_resource.hpp>

memory_pool::PoolMemoryResource resource;
std::pmr::vector<int> values(&resource);
```

The container must not outlive `resource`. Two distinct pool resources compare
unequal because neither can release the other's allocations.

## Shared allocation

```cpp
memory_pool::ThreadCachedAllocator allocator;
void* memory = allocator.allocate(48, 16);
allocator.deallocate(memory, 48, 16);
```

Same-thread frees may enter a local cache. Cross-thread frees are supported and
return directly to synchronized central storage. Join all worker threads before
destroying the allocator. Use `release_current_thread_cache()` when a long-lived
thread should return its cached capacity early.

## Lifetime arena

```cpp
memory_pool::MonotonicArena arena;
Node* root = arena.create<Node>("root");
arena.reset();  // destroys registered objects and invalidates root
```

There is no individual arena deallocation. Non-trivial objects created through
`create<T>()` are destroyed in reverse order. Raw `allocate()` does not register
a destructor. Arena-managed destructors must be `noexcept`.

## Diagnostics

```cpp
memory_pool::PoolOptions options{
    .diagnostics = memory_pool::DiagnosticMode::enabled,
    .poison_memory = true,
    .guard_bytes = true,
    .collect_statistics = true,
};
```

Diagnostics enable deterministic block-state checks. Poisoning helps identify
uninitialized or released memory in a debugger. Guards detect writes immediately
outside the payload when the block is returned. These features add overhead and
are intended primarily for development and validation.
