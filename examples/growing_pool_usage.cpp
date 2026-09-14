#include "memory_pool/chunk_pool.hpp"

#include <cstddef>
#include <iostream>
#include <vector>

int main() {
    memory_pool::GrowingChunkPool pool({
        .block_size = 64,
        .initial_blocks = 2,
        .alignment = alignof(std::max_align_t),
        .growth = memory_pool::GrowthPolicy::geometric(),
        .reclamation = {.spare_empty_chunks = 1},
    });

    std::vector<void*> allocations;
    // Five live blocks exceed capacities 2 then 4, demonstrating growth.
    for (int index = 0; index < 5; ++index) {
        allocations.push_back(pool.allocate());
    }

    std::cout << "chunks after growth=" << pool.chunk_count() << '\n';
    std::cout << "capacity after growth=" << pool.capacity() << '\n';

    // Completely empty excess chunks are released, while one spare remains.
    for (void* pointer : allocations) {
        pool.deallocate(pointer);
    }
    std::cout << "chunks after reclamation=" << pool.chunk_count() << '\n';
}
