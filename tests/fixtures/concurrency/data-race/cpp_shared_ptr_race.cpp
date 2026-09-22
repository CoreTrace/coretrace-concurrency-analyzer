// SPDX-License-Identifier: Apache-2.0
// Test: C++ - Data race sur un std::shared_ptr partagé sans synchronisation
//
// Bien que le compteur de références de shared_ptr soit atomique, les opérations
// sur le pointeur lui-même (copy, reset, operator=) ne sont PAS thread-safe.
// L'accès concurrent à la même instance de shared_ptr sans mutex est UB.
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

struct Config {
    int value = 0;
};

std::shared_ptr<Config> g_config;  // Partagé sans protection

void writer() {
    // DATA RACE: reset() modifie le pointeur interne et le compteur simultanément
    g_config.reset(new Config{42});
}

void reader() {
    // DATA RACE: copier un shared_ptr pendant qu'un autre thread appelle reset()
    // accède aux champs internes du bloc de contrôle de façon non atomique
    std::shared_ptr<Config> local = g_config;
    if (local) {
        (void)local->value;
    }
}

int main() {
    g_config = std::make_shared<Config>();

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++) {
        threads.emplace_back(writer);
        threads.emplace_back(reader);
    }
    for (auto& t : threads) {
        t.join();
    }
    return 0;
}
