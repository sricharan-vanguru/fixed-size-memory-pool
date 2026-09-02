#include "memory_pool/fixed_size_memory_pool.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct Tracked {
    static inline int alive = 0;
    int value;

    explicit Tracked(int input) : value(input) { ++alive; }
    ~Tracked() { --alive; }
};

struct Throwing {
    Throwing() { throw std::runtime_error("construction failed"); }
};

void test_exhaustion_and_reuse() {
    memory_pool::FixedSizeMemoryPool pool(24, 3);
    void* first = pool.allocate();
    void* second = pool.allocate();
    void* third = pool.allocate();

    expect(first != nullptr && second != nullptr && third != nullptr,
           "all configured blocks should be available");
    expect(pool.allocate() == nullptr, "an exhausted pool should return nullptr");
    expect(pool.available() == 0, "available count should reach zero");

    pool.deallocate(second);
    expect(pool.allocate() == second, "the free list should reuse the released block in O(1)");
}

void test_lifetime_management() {
    memory_pool::FixedSizeMemoryPool pool(sizeof(Tracked), 2, alignof(Tracked));
    Tracked* value = pool.create<Tracked>(99);
    expect(value->value == 99 && Tracked::alive == 1, "create should construct the object");
    pool.destroy(value);
    expect(Tracked::alive == 0 && pool.available() == 2,
           "destroy should run the destructor and release the block");
}

void test_constructor_exception_releases_block() {
    memory_pool::FixedSizeMemoryPool pool(sizeof(Throwing), 1);
    try {
        static_cast<void>(pool.create<Throwing>());
    } catch (const std::runtime_error&) {
    }
    expect(pool.available() == 1, "failed construction must not leak a block");
}

void test_alignment() {
    constexpr std::size_t alignment = 64;
    memory_pool::FixedSizeMemoryPool pool(17, 8, alignment);
    std::vector<void*> blocks;
    for (std::size_t index = 0; index < pool.capacity(); ++index) {
        void* pointer = pool.allocate();
        expect(reinterpret_cast<std::uintptr_t>(pointer) % alignment == 0,
               "every block should satisfy the requested alignment");
        blocks.push_back(pointer);
    }
    for (void* pointer : blocks) {
        pool.deallocate(pointer);
    }
}

void test_rejects_invalid_pointer() {
    memory_pool::FixedSizeMemoryPool pool(16, 2);
    int external = 0;
    try {
        pool.deallocate(&external);
        expect(false, "foreign pointer should be rejected");
    } catch (const std::invalid_argument&) {
    }
}

}  // namespace

int main() {
    test_exhaustion_and_reuse();
    test_lifetime_management();
    test_constructor_exception_releases_block();
    test_alignment();
    test_rejects_invalid_pointer();

    if (failures == 0) {
        std::cout << "All memory pool tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}

