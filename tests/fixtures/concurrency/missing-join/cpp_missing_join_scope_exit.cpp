// SPDX-License-Identifier: Apache-2.0
// Test: C++ - std::thread sort de portée sans join ni detach -> std::terminate
#include <thread>
#include <iostream>

void risky_operation(bool should_throw) {
    std::thread worker([]() {
        for (volatile int i = 0; i < 100000; i++);
        std::cout << "Worker terminé\n";
    });

    if (should_throw) {
        // MISSING JOIN: worker sort de portée ici sans join/detach -> std::terminate()
        std::cout << "Abandon anticipé\n";
        return;
    }

    worker.join();
}

int main() {
    risky_operation(true);
    std::cout << "Main terminé\n";
    return 0;
}
