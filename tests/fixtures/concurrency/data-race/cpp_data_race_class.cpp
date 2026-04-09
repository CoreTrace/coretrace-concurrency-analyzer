// SPDX-License-Identifier: Apache-2.0
// Test 7: C++ - Data race dans une classe
#include <thread>
#include <vector>
#include <iostream>

class Counter {
private:
    int value = 0;  // Non protégé

public:
    void increment() {
        value++;  // DATA RACE: non thread-safe
    }

    int get() const {
        return value;  // DATA RACE: lecture concurrente
    }
};

Counter global_counter;

void worker(int iterations) {
    for (int i = 0; i < iterations; i++) {
        global_counter.increment();
    }
}

int main() {
    std::vector<std::thread> threads;

    for (int i = 0; i < 4; i++) {
        threads.emplace_back(worker, 10000);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "Final value: " << global_counter.get()
              << " (expected: 40000)" << std::endl;

    return 0;
}
