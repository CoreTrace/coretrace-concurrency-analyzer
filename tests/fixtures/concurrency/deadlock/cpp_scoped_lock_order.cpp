// SPDX-License-Identifier: Apache-2.0
// Test: C++ - ABBA deadlock avec std::lock_guard dans ordre inverse
//
// Deux threads acquièrent deux std::mutex dans l'ordre opposé en utilisant
// std::lock_guard. Sans std::lock() ou std::scoped_lock pour l'acquisition
// simultanée, un deadlock circulaire peut se produire si chaque thread
// détient un verrou et attend l'autre.
#include <iostream>
#include <mutex>
#include <thread>

std::mutex mutex_a;
std::mutex mutex_b;

int account_a = 1000;
int account_b = 2000;

void transfer_a_to_b(int amount) {
    std::lock_guard<std::mutex> la(mutex_a);  // Acquiert A en premier
    // Simule un délai entre les deux acquisitions
    for (volatile int i = 0; i < 50000; i++);
    std::lock_guard<std::mutex> lb(mutex_b);  // Puis B
    // DEADLOCK: si transfer_b_to_a détient mutex_b et attend mutex_a

    account_a -= amount;
    account_b += amount;
    std::cout << "A->B: " << amount << "\n";
}

void transfer_b_to_a(int amount) {
    std::lock_guard<std::mutex> lb(mutex_b);  // Acquiert B en premier (ORDRE INVERSE)
    for (volatile int i = 0; i < 50000; i++);
    std::lock_guard<std::mutex> la(mutex_a);  // Puis A
    // DEADLOCK: si transfer_a_to_b détient mutex_a et attend mutex_b

    account_b -= amount;
    account_a += amount;
    std::cout << "B->A: " << amount << "\n";
}

int main() {
    std::thread t1(transfer_a_to_b, 100);
    std::thread t2(transfer_b_to_a, 200);

    t1.join();
    t2.join();

    std::cout << "A=" << account_a << " B=" << account_b << "\n";
    return 0;
}
