// SPDX-License-Identifier: Apache-2.0
// Test: Atomic ordering - seqlock avec lectures memory_order_relaxed côté lecteur
//
// Un seqlock correct exige que le lecteur utilise memory_order_acquire sur les
// lectures du compteur de séquence pour établir un happens-before avec les
// écritures du producteur (memory_order_release). Sans acquire, le CPU peut
// réordonner les lectures de données AVANT la lecture du compteur s1, ou APRÈS
// la lecture de s2 — rendant possible une lecture déchirée (x != y) même si
// le compteur semble cohérent.
#include <atomic>
#include <iostream>
#include <thread>

static std::atomic<unsigned> seq{0};  // Pair = stable, impair = écriture en cours
static int data_x = 0;
static int data_y = 0;
static std::atomic<bool> done{false};

void writer() {
    for (int i = 0; i < 1000; i++) {
        seq.fetch_add(1, std::memory_order_release);  // Passe à impair (écriture)
        data_x = i;
        data_y = i * 2;                               // x et y toujours cohérents ensemble
        seq.fetch_add(1, std::memory_order_release);  // Repasse à pair (stable)
    }
    done.store(true, std::memory_order_relaxed);
}

void reader() {
    int torn_count = 0;
    while (!done.load(std::memory_order_relaxed)) {
        unsigned s1, s2;
        int rx, ry;
        do {
            // ATOMIC ORDERING: devrait être memory_order_acquire pour établir
            // happens-before avec le store release du writer
            s1 = seq.load(std::memory_order_relaxed);
            // ATOMIC ORDERING: sans acquire fence ici, le CPU peut spéculativement
            // lire data_x et data_y AVANT la lecture de s1 (valeurs d'une version
            // précédente ou en cours d'écriture)
            rx = data_x;
            ry = data_y;
            s2 = seq.load(std::memory_order_relaxed);  // ATOMIC ORDERING: idem
        } while ((s1 & 1) || s1 != s2);

        // Lecture déchirée possible : rx = i, ry = (i-1)*2 d'une itération précédente
        if (rx * 2 != ry) {
            torn_count++;
        }
    }
    if (torn_count > 0) {
        std::cout << "Lectures déchirées détectées : " << torn_count << "\n";
    }
}

int main() {
    std::thread tw(writer);
    std::thread tr(reader);
    tw.join();
    tr.join();
    return 0;
}
