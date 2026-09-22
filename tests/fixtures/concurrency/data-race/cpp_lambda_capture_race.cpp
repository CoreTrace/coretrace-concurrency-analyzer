// SPDX-License-Identifier: Apache-2.0
// Test: C++ - Data race via capture par référence dans un lambda passé à std::thread
//
// Un lambda capture une variable locale par référence, puis est passé à plusieurs
// std::thread. Les threads peuvent survivre à la portée de la variable capturée
// (si mal synchronisés) et plusieurs threads écrivent/lisent la même référence
// simultanément sans protection : data race garantie.
#include <iostream>
#include <thread>
#include <vector>

void launch_workers() {
    int counter = 0;  // Variable locale capturée par référence

    std::vector<std::thread> threads;
    for (int i = 0; i < 6; i++) {
        // DATA RACE: chaque thread capture &counter et y accède concurremment
        threads.emplace_back([&counter, i]() {
            for (int j = 0; j < 1000; j++) {
                counter++;  // DATA RACE: lecture-modification-écriture non atomique
            }
        });
    }

    // Si join() était omis ici, counter deviendrait un pointeur pendant que les
    // threads tournent encore — dangling reference. Même avec join(), les écritures
    // concurrentes sans mutex constituent une data race.
    for (auto& t : threads) {
        t.join();
    }
    std::cout << "counter = " << counter << " (attendu 6000)\n";
}

int main() {
    launch_workers();
    return 0;
}
