// SPDX-License-Identifier: Apache-2.0
// Test: Atomic ordering - réordering store-load (test de Dekker avec relaxed)
//
// Chaque thread écrit son propre drapeau puis lit celui de l'autre.
// Avec memory_order_relaxed, le CPU (ARM, POWER) peut réordonner le store local
// avant le load distant : les deux threads peuvent lire 0 simultanément et
// entrer dans leur section critique en parallèle — violant l'exclusion mutuelle.
// Correction : utiliser memory_order_seq_cst ou une barrière std::atomic_thread_fence.
#include <atomic>
#include <iostream>
#include <thread>

std::atomic<int> flag_a{0};
std::atomic<int> flag_b{0};
int shared = 0;  // Doit être accédé par un seul thread à la fois

void thread_a() {
    flag_a.store(1, std::memory_order_relaxed);
    // ATOMIC ORDERING: aucune barrière entre le store et le load suivant.
    // Le CPU peut réordonner : load(flag_b) avant store(flag_a).
    if (flag_b.load(std::memory_order_relaxed) == 0) {
        // DATA RACE: thread_b peut aussi lire flag_a == 0 et entrer ici
        shared++;
    }
}

void thread_b() {
    flag_b.store(1, std::memory_order_relaxed);
    // ATOMIC ORDERING: même problème symétrique
    if (flag_a.load(std::memory_order_relaxed) == 0) {
        // DATA RACE: thread_a peut aussi être dans sa section critique
        shared++;
    }
}

int main() {
    // Répète l'expérience : sur ARM/POWER, shared == 2 est observable
    // (les deux threads entrent simultanément dans leur section critique)
    for (int run = 0; run < 10; run++) {
        flag_a.store(0, std::memory_order_relaxed);
        flag_b.store(0, std::memory_order_relaxed);
        shared = 0;

        std::thread ta(thread_a);
        std::thread tb(thread_b);
        ta.join();
        tb.join();

        if (shared == 2) {
            std::cout << "Violation d'exclusion mutuelle détectée (run " << run << ")\n";
        }
    }
    std::cout << "shared final = " << shared << "\n";
    return 0;
}
