#include "memory_pool/segregated_allocator.hpp"

#include <cstddef>
#include <iostream>

int main() {
    memory_pool::SegregatedAllocator allocator;

    void* small = allocator.allocate(24, 8);
    void* aligned = allocator.allocate(48, 64);
    void* large = allocator.allocate(8192, alignof(std::max_align_t));

    std::cout << "24-byte request uses class "
              << *allocator.owning_size_class(small) << '\n';
    std::cout << "48-byte aligned request uses class "
              << *allocator.owning_size_class(aligned) << '\n';
    std::cout << "8192-byte request uses system fallback="
              << !allocator.owning_size_class(large).has_value() << '\n';

    allocator.deallocate(small, 24, 8);
    allocator.deallocate(aligned, 48, 64);
    allocator.deallocate(large, 8192, alignof(std::max_align_t));
}
