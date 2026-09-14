#include <memory_pool/object_pool.hpp>

int main() {
    memory_pool::ObjectPool<int> pool(1);
    auto value = pool.make_unique(42);
    return *value == 42 ? 0 : 1;
}
