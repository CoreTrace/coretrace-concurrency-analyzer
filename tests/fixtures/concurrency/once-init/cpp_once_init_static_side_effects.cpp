// SPDX-License-Identifier: Apache-2.0
// Test: C++ Once-init - initialisation de static local avec effets de bord concurrents
#include <iostream>
#include <thread>
#include <vector>

// Simule un registre global modifié comme effet de bord de l'initialisation
static int registry_count = 0;

struct Service {
    int id;
    Service() {
        // ONCE-INIT: constructeur avec effets de bord - registry_count non protégé
        registry_count++;          // DATA RACE si plusieurs threads initialisent simultanément
        id = registry_count;
        std::cout << "Service créé avec id=" << id << "\n";
    }
};

// En C++11, l'init de static local est thread-safe, MAIS les effets de bord
// sur registry_count (variable externe non protégée) ne le sont pas
Service& get_service() {
    static Service instance;  // Init thread-safe, mais effets de bord sur registry_count non protégés
    return instance;
}

void worker() {
    Service& s = get_service();
    registry_count += s.id;  // DATA RACE: accès concurrent à registry_count
}

int main() {
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; i++) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }
    std::cout << "Registry count final: " << registry_count << "\n";
    return 0;
}
