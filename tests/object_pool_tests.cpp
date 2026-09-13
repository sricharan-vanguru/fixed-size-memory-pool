#include "memory_pool/object_pool.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace object_pool_tests {
namespace {

struct Tracked {
    static inline int alive = 0;
    int value;

    explicit Tracked(int input) : value(input) { ++alive; }
    ~Tracked() { --alive; }
};

struct alignas(64) OverAligned {
    int value;
};

struct MoveOnly {
    explicit MoveOnly(std::unique_ptr<int> input) : value(std::move(input)) {}

    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;

    std::unique_ptr<int> value;
};

struct Throwing {
    Throwing() { throw std::runtime_error("construction failed"); }
};

static_assert(!std::is_copy_constructible_v<memory_pool::ObjectPool<int>>);
static_assert(!std::is_move_constructible_v<memory_pool::ObjectPool<int>>);
static_assert(
    std::is_same_v<memory_pool::ObjectPool<int>::value_type, int>);

void typed_creation_and_capacity(TestContext& test) {
    memory_pool::ObjectPool<Tracked> pool(2, diagnostic_options(true));
    Tracked* first = pool.create(10);
    Tracked* second = pool.create(20);

    test.expect(first->value == 10 && second->value == 20,
                "ObjectPool should forward constructor arguments");
    test.expect(pool.capacity() == 2 && pool.available() == 0 && pool.in_use() == 2,
                "ObjectPool should expose core capacity information");
    test.expect(pool.owns(first), "ObjectPool should recognize its object addresses");
    test.expect(pool.statistics().peak_allocated == 2,
                "ObjectPool should expose optional core statistics");

    pool.destroy(second);
    pool.destroy(first);
    test.expect(Tracked::alive == 0 && pool.available() == 2,
                "typed destruction should release every object");
}

void over_aligned_type(TestContext& test) {
    memory_pool::ObjectPool<OverAligned> pool(2);
    OverAligned* object = pool.create();
    test.expect(reinterpret_cast<std::uintptr_t>(object) % alignof(OverAligned) == 0,
                "ObjectPool should derive alignment from T");
    pool.destroy(object);
}

void move_only_constructor_argument(TestContext& test) {
    memory_pool::ObjectPool<MoveOnly> pool(1);
    MoveOnly* object = pool.create(std::make_unique<int>(42));
    test.expect(object->value != nullptr && *object->value == 42,
                "ObjectPool should perfectly forward move-only arguments");
    pool.destroy(object);
}

void throwing_constructor(TestContext& test) {
    memory_pool::ObjectPool<Throwing> pool(1);
    test.expect_throws<std::runtime_error>(
        [&] { static_cast<void>(pool.make_unique()); },
        "make_unique should preserve constructor exceptions");
    test.expect(pool.available() == 1,
                "ObjectPool should return a block after construction fails");
}

void raii_scope_exit(TestContext& test) {
    memory_pool::ObjectPool<Tracked> pool(1, diagnostic_options());
    {
        auto object = pool.make_unique(7);
        test.expect(object->value == 7 && Tracked::alive == 1,
                    "make_unique should return an owning pooled pointer");
        test.expect(pool.available() == 0, "a PoolPtr should hold one pool block");
    }
    test.expect(Tracked::alive == 0 && pool.available() == 1,
                "PoolPtr should destroy and release its object at scope exit");
}

void pooled_pointer_move_and_null(TestContext& test) {
    memory_pool::ObjectPool<Tracked> pool(1, diagnostic_options());
    memory_pool::PoolPtr<Tracked> empty;
    test.expect(empty == nullptr, "a default PoolPtr should be empty");

    auto first = pool.make_unique(9);
    memory_pool::PoolPtr<Tracked> second = std::move(first);
    test.expect(first == nullptr && second != nullptr,
                "moving PoolPtr should transfer sole ownership");
    second.reset();
    test.expect(Tracked::alive == 0 && pool.available() == 1,
                "reset should destroy the object exactly once");
}

void exception_unwinding(TestContext& test) {
    memory_pool::ObjectPool<Tracked> pool(1, diagnostic_options());
    try {
        auto object = pool.make_unique(11);
        throw std::runtime_error("later operation failed");
    } catch (const std::runtime_error&) {
    }

    test.expect(Tracked::alive == 0 && pool.available() == 1,
                "PoolPtr should release its object during stack unwinding");
}

void smart_pointer_exhaustion(TestContext& test) {
    memory_pool::ObjectPool<Tracked> pool(1);
    auto first = pool.make_unique(1);
    test.expect_throws<std::bad_alloc>(
        [&] { static_cast<void>(pool.make_unique(2)); },
        "make_unique should report pool exhaustion");
}

}  // namespace

void run(TestContext& test) {
    typed_creation_and_capacity(test);
    over_aligned_type(test);
    move_only_constructor_argument(test);
    throwing_constructor(test);
    raii_scope_exit(test);
    pooled_pointer_move_and_null(test);
    exception_unwinding(test);
    smart_pointer_exhaustion(test);
}

}  // namespace object_pool_tests
