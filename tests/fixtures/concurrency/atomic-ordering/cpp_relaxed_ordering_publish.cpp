// SPDX-License-Identifier: Apache-2.0
// Test: Atomic ordering - utilisation de memory_order_relaxed insuffisant pour synchroniser
#include <atomic>
#include <thread>
#include <iostream>

std::atomic<int> data{0};
std::atomic<bool> ready{false};

void producer() {
    data.store(42, std::memory_order_relaxed);   // ATOMIC ORDERING: relaxed ne garantit pas
    ready.store(true, std::memory_order_relaxed); // l'ordre d'observation avec data
}

void consumer() {
    while (!ready.load(std::memory_order_relaxed)) {
        // Spin-wait avec relaxed - aucune garantie de voir data=42 quand ready=true
    }
    // ATOMIC ORDERING: peut voir ready=true mais data=0 avec memory_order_relaxed
    std::cout << "Data: " << data.load(std::memory_order_relaxed)
              << " (attendu: 42)\n";
}

int main() {
    std::thread t1(producer);
    std::thread t2(consumer);
    t1.join();
    t2.join();
    return 0;
}
