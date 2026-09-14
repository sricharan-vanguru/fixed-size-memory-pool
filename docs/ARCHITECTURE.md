# Architecture

## Purpose

This project separates several allocation models instead of hiding them behind
one large class. Each public type has one memory-lifetime or synchronization
contract, allowing callers to pay only for the behavior they select.

The library is educational and suitable for controlled workloads. It is not a
drop-in replacement for every general-purpose allocator.

## Layers

| Layer | Main types | Responsibility |
|---|---|---|
| Backing storage | `IMemoryProvider`, `NewDeleteMemoryProvider` | Acquire and release aligned byte regions |
| Fixed primitive | `FixedSizeMemoryPool` | Reuse equal-size blocks through an intrusive free list |
| Typed ownership | `ObjectPool<T>`, `PoolPtr<T>` | Construct, destroy, and scope objects in fixed blocks |
| Stable growth | `MemoryChunk`, `ChunkManager`, `BasicChunkPool` | Add non-moving chunks and select exhaustion behavior |
| Mixed sizes | `SizeClassSelector`, `SegregatedAllocator` | Route requests to size classes or provider fallback |
| Standard adapter | `PoolMemoryResource` | Expose the allocator through `std::pmr::memory_resource` |
| Shared access | `SynchronizedAllocator`, `ThreadCachedAllocator` | Serialize central state and optionally batch per-thread reuse |
| Lifetime allocation | `MonotonicArena` | Allocate forward and reclaim one complete lifetime with `reset()` |

Normal fixed-pool allocation is a free-list pop. A returned block becomes the
new list head, so the next allocation reuses it in LIFO order. Free blocks store
their own `next` pointer; live blocks contain only caller data.

Mixed-size requests take a different path:

```text
request(size, alignment)
        |
        v
smallest compatible size class? -- no --> exact provider fallback
        |
       yes
        v
existing chunk --> grow if full --> return stable block
```

## Design patterns

- **Strategy:** memory providers, growth settings, reclamation settings, and
  exhaustion policies vary independently.
- **Adapter:** `PoolMemoryResource` adapts `SegregatedAllocator` to PMR without
  changing either contract.
- **Decorator:** synchronized and thread-cached allocators add concurrency above
  the unsynchronized allocator.
- **Facade:** `ObjectPool<T>` removes manual size/alignment configuration for a
  single object type.
- **RAII:** `PoolPtr<T>`, chunks, providers, and implementation objects release
  owned resources during destruction.
- **PIMPL:** larger non-template public types hide implementation details and
  keep private headers out of the supported API.

## Ownership and lifetime

- Pools, allocators, PMR resources, and arenas are non-copyable and non-movable
  because moving their state could invalidate ownership relationships.
- A pool must outlive every raw pointer and `PoolPtr` created from it.
- PMR containers must be destroyed before their `PoolMemoryResource`.
- A custom PMR upstream resource is non-owning and must outlive the adapter.
- `MonotonicArena::reset()` invalidates every pointer from that arena.
- Worker threads must finish before a shared allocator is destroyed.

## Error and exception model

Invalid configuration is rejected before backing storage is acquired. Foreign,
interior, duplicate, and mismatched returns throw project-specific exceptions
where the selected diagnostics contain enough information to identify them.

Provider acquisition and object construction failures preserve existing live
allocations. If metadata insertion fails after a provider allocation succeeds,
the new allocation is returned immediately. Destructors and provider
deallocation functions are required not to throw.

## Concurrency model

The fixed pool, chunk manager, segregated allocator, PMR adapter, and arena are
unsynchronized. `SynchronizedAllocator` places one lock around the complete
segregated operation.

`ThreadCachedAllocator` keeps private bins per thread and size class. Central
storage is protected by a mutex, while live-allocation records are split across
64 lock shards. Same-thread frees may remain cached; cross-thread frees bypass
foreign caches and return directly to central storage. The implementation makes
no lock-free progress guarantee.

## Public and private boundaries

Only headers installed under `include/memory_pool` are public. Files under
`src/detail` may change without compatibility guarantees. Consumers should link
the exported CMake target `memory_pool::memory_pool` instead of depending on
source files or private include paths.
