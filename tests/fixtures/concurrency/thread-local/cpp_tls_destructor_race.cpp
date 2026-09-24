// SPDX-License-Identifier: Apache-2.0
// Test: Thread-local - destructeur d'objet thread_local modifiant un global partagé
//
// Un objet thread_local a un constructeur/destructeur qui mettent à jour un
// compteur global non atomique. Le destructeur s'exécute à la terminaison du
// thread, sans synchronisation avec les autres threads en cours d'exécution.
// Plusieurs threads peuvent terminer simultanément, leurs destructeurs TLS
// faisant des read-modify-write concurrents sur le même compteur global.
#include <iostream>
#include <thread>
#include <vector>

static int active_count = 0;  // DATA RACE: modifié par les destructeurs TLS sans protection

struct ThreadTracker {
    ThreadTracker()  { active_count++; }  // DATA RACE: appelé au premier accès TLS du thread
    ~ThreadTracker() {
        // DATA RACE: s'exécute à la fin du thread, en parallèle avec d'autres
        // destructeurs TLS ou avec des lectures de active_count dans main
        active_count--;
    }
};

void worker() {
    // L'objet TLS est construit au premier passage ici et détruit à la fin du thread
    thread_local ThreadTracker tracker;
    for (volatile int i = 0; i < 10000; i++);
}

int main() {
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; i++) {
        threads.emplace_back(worker);
    }

    // DATA RACE: lit active_count pendant que des threads terminent
    // et que leurs destructeurs TLS le décrémentent sans synchronisation
    for (int i = 0; i < 8; i++) {
        std::cout << "active_count snapshot: " << active_count << "\n";  // DATA RACE
        threads[i].join();
    }

    std::cout << "final active_count: " << active_count << "\n";
    return 0;
}
