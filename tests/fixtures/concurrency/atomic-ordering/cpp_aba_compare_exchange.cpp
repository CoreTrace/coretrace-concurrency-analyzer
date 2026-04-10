// SPDX-License-Identifier: Apache-2.0
// Test: Atomic ordering - problème ABA avec compare_exchange sur une valeur partagée
#include <atomic>
#include <thread>
#include <iostream>

std::atomic<int> shared_val{0};

// Simule un incrément conditionnel: applique seulement si la valeur n'a pas changé
// Vulnérable à ABA: val passe de 0 -> 1 -> 0 entre lecture et CAS
void try_increment(int expected) {
    int old = shared_val.load(std::memory_order_relaxed);
    // Simulation de délai - un autre thread peut changer val et le remettre à expected
    for (volatile int i = 0; i < 1000; i++);
    // ATOMIC ORDERING / ABA: CAS réussit même si la valeur a changé entre-temps
    if (shared_val.compare_exchange_weak(old, old + 1,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
        std::cout << "Incrémenté de " << old << " à " << old + 1 << "\n";
    }
}

void flipper() {
    shared_val.store(1, std::memory_order_relaxed);
    shared_val.store(0, std::memory_order_relaxed);  // ABA: remet à 0
}

int main() {
    std::thread t1([]{ try_increment(0); });
    std::thread t2(flipper);
    t1.join();
    t2.join();
    std::cout << "Valeur finale: " << shared_val.load() << "\n";
    return 0;
}
