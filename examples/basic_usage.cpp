#include "memory_pool/fixed_size_memory_pool.hpp"

#include <iostream>
#include <string>

struct Session {
    int id;
    std::string state;
};

int main() {
    memory_pool::FixedSizeMemoryPool pool(sizeof(Session), 4, alignof(Session));

    // create combines block allocation with placement construction.
    Session* session = pool.create<Session>(42, "active");
    std::cout << "session=" << session->id << ", state=" << session->state << '\n';
    std::cout << "available blocks=" << pool.available() << '\n';

    // destroy runs Session's destructor before returning its block.
    pool.destroy(session);
    std::cout << "available after release=" << pool.available() << '\n';
}
