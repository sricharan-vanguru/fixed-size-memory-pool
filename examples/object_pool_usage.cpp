#include "memory_pool/object_pool.hpp"

#include <iostream>
#include <string>

struct Session {
    int id;
    std::string state;
};

int main() {
    memory_pool::ObjectPool<Session> sessions(4);

    {
        // PoolPtr automatically destroys the object and returns the block when
        // this scope ends. The pool itself therefore has to be declared first.
        auto session = sessions.make_unique(42, "active");
        std::cout << "session=" << session->id << ", state=" << session->state << '\n';
        std::cout << "available blocks=" << sessions.available() << '\n';
    }

    std::cout << "available after scope=" << sessions.available() << '\n';
}
