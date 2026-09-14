#pragma once

#include "memory_pool/arena_options.hpp"
#include "memory_pool/arena_statistics.hpp"
#include "memory_pool/memory_provider.hpp"

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace memory_pool {

/// Unsynchronized lifetime arena. Raw allocations have no individual free;
/// objects created with create<T>() are destroyed in reverse order by reset().
class MonotonicArena {
  public:
    explicit MonotonicArena(MonotonicArenaOptions options = {},
                            MemoryProviderPtr provider = {});
    ~MonotonicArena();

    MonotonicArena(const MonotonicArena&) = delete;
    MonotonicArena& operator=(const MonotonicArena&) = delete;
    MonotonicArena(MonotonicArena&&) = delete;
    MonotonicArena& operator=(MonotonicArena&&) = delete;

    [[nodiscard]] void* allocate(std::size_t size,
                                 std::size_t alignment = alignof(std::max_align_t));

    /// Allocates and constructs one object. Non-trivial destructors run during
    /// reset or arena destruction; individual destruction is not supported.
    template <typename T, typename... Args> [[nodiscard]] T* create(Args&&... args) & {
        static_assert(std::is_nothrow_destructible_v<T>,
                      "arena-managed objects must have noexcept destructors");

        void* const storage = allocate(sizeof(T), alignof(T));
        T* object = nullptr;
        try {
            object = std::construct_at(static_cast<T*>(storage),
                                       std::forward<Args>(args)...);
            if constexpr (!std::is_trivially_destructible_v<T>) {
                // Trivial types need no record, keeping create<int>() cheap.
                register_destructor(object, [](void* pointer) noexcept {
                    std::destroy_at(static_cast<T*>(pointer));
                });
            }
            note_object_construction();
            return object;
        } catch (...) {
            // Registration itself may allocate metadata and throw after T was
            // constructed, so destroy it before attempting cursor rollback.
            if (object != nullptr) {
                std::destroy_at(object);
            }
            rollback_last_allocation(storage);
            throw;
        }
    }

    template <typename T, typename... Args> T* create(Args&&...) && = delete;

    /// Invalidates every arena pointer after running registered destructors.
    void reset() noexcept;

    /// Reports backing-storage membership, not whether an object is still live.
    [[nodiscard]] bool owns(const void* pointer) const noexcept;
    [[nodiscard]] std::size_t chunk_count() const noexcept;
    [[nodiscard]] std::size_t bytes_used() const noexcept;
    [[nodiscard]] std::size_t bytes_reserved() const noexcept;
    [[nodiscard]] const ArenaStatistics& statistics() const noexcept;

  private:
    using Destructor = void (*)(void*) noexcept;

    void register_destructor(void* object, Destructor destructor);
    void rollback_last_allocation(void* pointer) noexcept;
    void note_object_construction() noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace memory_pool
