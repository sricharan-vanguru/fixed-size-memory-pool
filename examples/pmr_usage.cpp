#include "memory_pool/pool_memory_resource.hpp"

#include <iostream>
#include <memory_resource>
#include <string>
#include <unordered_map>
#include <vector>

int main() {
    memory_pool::PoolMemoryResource resource;

    {
        // Containers must be destroyed before the resource that owns their
        // allocations; the nested scope makes that lifetime order explicit.
        std::pmr::vector<int> values(&resource);
        std::pmr::string message(&resource);
        std::pmr::unordered_map<int, std::pmr::string> labels(&resource);

        values.assign({10, 20, 30, 40});
        message = "containers allocate through PoolMemoryResource";
        labels.emplace(1, "pooled");

        std::cout << message << '\n';
        std::cout << "vector total=" << values[0] + values[1] + values[2] + values[3]
                  << '\n';
        std::cout << "label=" << labels.at(1) << '\n';
    }

    const auto& statistics = resource.statistics();
    std::cout << "PMR allocations=" << statistics.successful_allocations << '\n';
    std::cout << "PMR deallocations=" << statistics.deallocations << '\n';
}
