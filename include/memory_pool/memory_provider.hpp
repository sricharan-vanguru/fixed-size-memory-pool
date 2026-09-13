#pragma once

#include <cstddef>
#include <memory>

namespace memory_pool {

/// Supplies aligned backing storage. Implementations must either return a
/// non-null pointer or throw; deallocate must not throw.
class IMemoryProvider {
public:
    virtual ~IMemoryProvider() = default;

    [[nodiscard]] virtual void* allocate(std::size_t bytes,
                                         std::size_t alignment) = 0;
    virtual void deallocate(void* memory,
                            std::size_t bytes,
                            std::size_t alignment) noexcept = 0;
};

using MemoryProviderPtr = std::shared_ptr<IMemoryProvider>;

}  // namespace memory_pool
