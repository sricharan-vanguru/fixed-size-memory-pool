#include "memory_pool/fixed_size_memory_pool.hpp"

#include <iostream>
#include <string>

struct Session {
    int id;
    std::string state;
};

int main() {
    memory_pool::FixedSizeMemoryPool pool(sizeof(Session), 4, alignof(Session));

    Session* session = pool.create<Session>(42, "active");
    std::cout << "session=" << session->id << ", state=" << session->state << '\n';
    std::cout << "available blocks=" << pool.available() << '\n';

    pool.destroy(session);
    std::cout << "available after release=" << pool.available() << '\n';
}

