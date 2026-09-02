#include "memory_pool/fixed_size_memory_pool.hpp"

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <memory>
#include <cstdint>
#include <vector>

struct alignas(64) CacheLineObject {
    std::byte payload[64];
};

template <typename Operation>
double measure_nanoseconds(std::size_t operations, Operation&& operation) {
    const auto start = std::chrono::steady_clock::now();
    operation();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double, std::nano>(elapsed).count() /
           static_cast<double>(operations);
}

int main() {
    constexpr std::size_t iterations = 1'000'000;
    memory_pool::FixedSizeMemoryPool pool(
        sizeof(CacheLineObject), 1, alignof(CacheLineObject));

    std::uintptr_t checksum = 0;
    const double pool_time = measure_nanoseconds(iterations, [&] {
        for (std::size_t index = 0; index < iterations; ++index) {
            void* pointer = pool.allocate();
            asm volatile("" : : "g"(pointer) : "memory");
            checksum ^= reinterpret_cast<std::uintptr_t>(pointer);
            pool.deallocate(pointer);
        }
    });

    const double heap_time = measure_nanoseconds(iterations, [&] {
        for (std::size_t index = 0; index < iterations; ++index) {
            auto* pointer = new CacheLineObject;
            asm volatile("" : : "g"(pointer) : "memory");
            checksum ^= reinterpret_cast<std::uintptr_t>(pointer);
            delete pointer;
        }
    });

    std::cout << std::fixed << std::setprecision(2)
              << "pool allocate/deallocate: " << pool_time << " ns/op\n"
              << "heap new/delete:          " << heap_time << " ns/op\n"
              << "checksum:                 " << checksum << '\n';
}
