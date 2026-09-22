// SPDX-License-Identifier: Apache-2.0
// Test: C++ - Deadlock par double acquisition d'un std::mutex non récursif
//
// Variante C++ de deadlock/recursive_deadlock.c (pthread_mutex_t).
// Le pattern est identique — outer acquiert le verrou, appelle inner qui tente
// de le réacquérir — mais l'IR LLVM généré pour std::mutex::lock() diffère
// de pthread_mutex_lock(), ce qui justifie les deux fichiers pour la couverture
// de l'analyseur.
#include <mutex>
#include <thread>
#include <iostream>

std::mutex mtx;

void inner_operation() {
    mtx.lock();    // DEADLOCK: tente d'acquérir un verrou déjà tenu par ce thread
    std::cout << "inner_operation exécutée\n";
    mtx.unlock();
}

void outer_operation() {
    mtx.lock();
    inner_operation();  // Appelle une fonction qui verrouille le même mutex
    mtx.unlock();
}

int main() {
    std::thread t(outer_operation);
    t.join();
    return 0;
}
