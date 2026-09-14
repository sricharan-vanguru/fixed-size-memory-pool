#include "memory_pool/monotonic_arena.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory_resource>
#include <vector>

namespace {

template <typename Operation>
double measure_nanoseconds(std::size_t operations, Operation&& operation) {
    const auto start = std::chrono::steady_clock::now();
    operation();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double, std::nano>(elapsed).count() /
           static_cast<double>(operations);
}

}  // namespace

int main() {
    constexpr std::size_t rounds = 100;
    constexpr std::size_t allocations_per_round = 10'000;
    constexpr std::size_t operations = rounds * allocations_per_round;
    constexpr std::size_t bytes_per_round = allocations_per_round * 32;
    std::uintptr_t checksum = 0;

    memory_pool::MonotonicArena arena({
        .initial_chunk_size = bytes_per_round,
        .growth = memory_pool::ArenaGrowthPolicy::fixed(),
        .reset_policy = memory_pool::ArenaResetPolicy::retain_all_chunks,
    });
    const double arena_time = measure_nanoseconds(operations, [&] {
        for (std::size_t round = 0; round < rounds; ++round) {
            for (std::size_t index = 0; index < allocations_per_round; ++index) {
                void* const pointer = arena.allocate(32, 16);
                checksum += reinterpret_cast<std::uintptr_t>(pointer);
            }
            arena.reset();
        }
    });

    std::vector<std::byte> standard_storage(bytes_per_round);
    std::pmr::monotonic_buffer_resource standard(
        standard_storage.data(),
        standard_storage.size(),
        std::pmr::null_memory_resource());
    const double standard_time = measure_nanoseconds(operations, [&] {
        for (std::size_t round = 0; round < rounds; ++round) {
            for (std::size_t index = 0; index < allocations_per_round; ++index) {
                void* const pointer = standard.allocate(32, 16);
                checksum += reinterpret_cast<std::uintptr_t>(pointer);
            }
            standard.release();
        }
    });

    std::cout << std::fixed << std::setprecision(2)
              << "custom monotonic arena:       " << arena_time << " ns/allocation\n"
              << "standard PMR monotonic arena: " << standard_time
              << " ns/allocation\n"
              << "checksum:                     " << checksum << '\n';
}
