#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace core_tests {
namespace {

// Core behavior: capacity, LIFO reuse, alignment, pointer checks, and lifetime.
struct Tracked {
    static inline int alive = 0;
    int value;

    explicit Tracked(int input) : value(input) { ++alive; }
    ~Tracked() { --alive; }
};

struct Throwing {
    Throwing() { throw std::runtime_error("construction failed"); }
};

struct ThrowingInteger {
    ThrowingInteger() { throw 7; }
};

struct LargeObject {
    std::byte data[128];
};

struct alignas(64) OverAlignedObject {
    int value;
};

void exhaustion_and_reuse(TestContext& test) {
    memory_pool::FixedSizeMemoryPool pool(24, 3);
    void* first = pool.allocate();
    void* second = pool.allocate();
    void* third = pool.allocate();

    test.expect(first != nullptr && second != nullptr && third != nullptr,
                "all configured blocks should be available");
    test.expect(pool.allocate() == nullptr, "an exhausted pool should return nullptr");
    test.expect(pool.available() == 0, "available count should reach zero");
    pool.deallocate(second);
    test.expect(pool.allocate() == second, "the released block should be reused in O(1)");
}

void lifetime_management(TestContext& test) {
    memory_pool::FixedSizeMemoryPool pool(sizeof(Tracked), 2, alignof(Tracked));
    Tracked* value = pool.create<Tracked>(99);
    test.expect(value->value == 99 && Tracked::alive == 1,
                "create should construct the object");
    pool.destroy(value);
    test.expect(Tracked::alive == 0 && pool.available() == 2,
                "destroy should run the destructor and release the block");
}

void constructor_exceptions(TestContext& test) {
    memory_pool::FixedSizeMemoryPool pool(sizeof(Throwing), 1);
    test.expect_throws<std::runtime_error>(
        [&] { static_cast<void>(pool.create<Throwing>()); },
        "create should rethrow standard constructor exceptions");
    test.expect(pool.available() == 1, "failed construction must not leak a block");

    test.expect_throws<int>(
        [&] { static_cast<void>(pool.create<ThrowingInteger>()); },
        "create should rethrow non-standard constructor exceptions");
    test.expect(pool.available() == 1,
                "all constructor exception types should return their block");
}

void alignment(TestContext& test) {
    constexpr std::size_t requested_alignment = 64;
    memory_pool::FixedSizeMemoryPool pool(17, 8, requested_alignment);
    std::vector<void*> blocks;
    for (std::size_t index = 0; index < pool.capacity(); ++index) {
        void* pointer = pool.allocate();
        test.expect(reinterpret_cast<std::uintptr_t>(pointer) % requested_alignment == 0,
                    "every block should satisfy the requested alignment");
        blocks.push_back(pointer);
    }
    for (void* pointer : blocks) {
        pool.deallocate(pointer);
    }
}

void constructor_validation(TestContext& test) {
    test.expect_throws<std::invalid_argument>(
        [] { memory_pool::FixedSizeMemoryPool pool(16, 0); },
        "zero capacity should be rejected");
    test.expect_throws<std::invalid_argument>(
        [] { memory_pool::FixedSizeMemoryPool pool(16, 1, 0); },
        "zero alignment should be rejected");
    test.expect_throws<std::invalid_argument>(
        [] { memory_pool::FixedSizeMemoryPool pool(16, 1, 3); },
        "non-power-of-two alignment should be rejected");
    test.expect_throws<std::invalid_argument>(
        [] {
            memory_pool::FixedSizeMemoryPool pool(
                16, 1, std::numeric_limits<std::size_t>::max());
        },
        "an extreme non-power-of-two alignment should be rejected");
    test.expect_throws<std::length_error>(
        [] {
            memory_pool::FixedSizeMemoryPool pool(
                std::numeric_limits<std::size_t>::max(), 1, 8);
        },
        "block-size rounding overflow should be rejected");
    test.expect_throws<std::length_error>(
        [] {
            memory_pool::FixedSizeMemoryPool pool(
                std::numeric_limits<std::size_t>::max() / 2U, 3, 8);
        },
        "total storage multiplication overflow should be rejected");
}

void pointer_validation(TestContext& test) {
    memory_pool::FixedSizeMemoryPool pool(32, 2);
    int external = 0;
    test.expect_throws<memory_pool::InvalidPoolPointer>(
        [&] { pool.deallocate(&external); }, "a foreign pointer should be rejected");

    pool.deallocate(nullptr);
    pool.destroy<int>(nullptr);
    test.expect(pool.available() == 2, "null cleanup should not change availability");

    void* block = pool.allocate();
    auto* interior = static_cast<std::byte*>(block) + 1;
    test.expect(!pool.is_block_start(interior), "an interior pointer is not a block start");
    test.expect_throws<memory_pool::InvalidPoolPointer>(
        [&] { pool.deallocate(interior); }, "an interior pointer should be rejected");
    pool.deallocate(block);
}

void type_validation_and_exhaustion(TestContext& test) {
    memory_pool::FixedSizeMemoryPool small_pool(16, 1);
    test.expect_throws<std::invalid_argument>(
        [&] { static_cast<void>(small_pool.create<LargeObject>()); },
        "an object larger than a block should be rejected");

    memory_pool::FixedSizeMemoryPool under_aligned_pool(128, 1, 16);
    test.expect_throws<std::invalid_argument>(
        [&] { static_cast<void>(under_aligned_pool.create<OverAlignedObject>()); },
        "an over-aligned object should be rejected by an under-aligned pool");

    memory_pool::FixedSizeMemoryPool pool(sizeof(Tracked), 1, alignof(Tracked));
    Tracked* object = pool.create<Tracked>(1);
    test.expect_throws<std::bad_alloc>(
        [&] { static_cast<void>(pool.create<Tracked>(2)); },
        "create should throw std::bad_alloc on exhaustion");
    pool.destroy(object);
}

void lifo_reuse(TestContext& test) {
    memory_pool::FixedSizeMemoryPool pool(16, 5);
    void* blocks[3]{pool.allocate(), pool.allocate(), pool.allocate()};
    pool.deallocate(blocks[1]);
    void* replacement = pool.allocate();
    test.expect(replacement == blocks[1],
                "the most recently released block should be allocated first");
    pool.deallocate(replacement);
    pool.deallocate(blocks[0]);
    pool.deallocate(blocks[2]);
}

void multiple_non_trivial_objects(TestContext& test) {
    memory_pool::FixedSizeMemoryPool pool(sizeof(Tracked), 3, alignof(Tracked),
                                          diagnostic_options());
    Tracked* first = pool.create<Tracked>(1);
    Tracked* second = pool.create<Tracked>(2);
    Tracked* third = pool.create<Tracked>(3);
    test.expect(Tracked::alive == 3, "three non-trivial objects should be alive");
    pool.destroy(second);
    pool.destroy(first);
    pool.destroy(third);
    test.expect(Tracked::alive == 0 && pool.available() == 3,
                "all destructors should run and return their blocks");
}

}  // namespace

void run(TestContext& test) {
    exhaustion_and_reuse(test);
    lifetime_management(test);
    constructor_exceptions(test);
    alignment(test);
    constructor_validation(test);
    pointer_validation(test);
    type_validation_and_exhaustion(test);
    lifo_reuse(test);
    multiple_non_trivial_objects(test);
}

}  // namespace core_tests
