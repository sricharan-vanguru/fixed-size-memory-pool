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
        auto session = sessions.make_unique(42, "active");
        std::cout << "session=" << session->id << ", state=" << session->state << '\n';
        std::cout << "available blocks=" << sessions.available() << '\n';
    }

    std::cout << "available after scope=" << sessions.available() << '\n';
}
