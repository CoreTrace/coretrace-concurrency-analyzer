// SPDX-License-Identifier: Apache-2.0
// Test: C++ - Invalidation d'itérateur via réallocation + data race sur std::vector
//
// Distinct de cpp_race_std_async.cpp (accès concurrent via std::async sur un vecteur
// partagé) : ici le mécanisme central est la RÉALLOCATION garantie. reserve(10) est
// délibérément insuffisant — dès que inserter() dépasse 10 éléments, push_back()
// réalloue le buffer interne et libère l'ancien bloc, invalidant tout itérateur
// encore détenu par iterator_reader(). L'accès via un itérateur invalidé est UB,
// en plus de la data race sur les champs internes du vecteur.
#include <iostream>
#include <thread>
#include <vector>

std::vector<int> shared_vec;  // Non protégé

void inserter() {
    for (int i = 0; i < 200; i++) {
        // DATA RACE: push_back peut réallouer, invalidant les itérateurs
        // de l'autre thread
        shared_vec.push_back(i);
    }
}

void iterator_reader() {
    // DATA RACE: itération pendant que l'autre thread insère et potentiellement
    // réalloue le buffer interne — accès à mémoire libérée possible
    for (auto it = shared_vec.begin(); it != shared_vec.end(); ++it) {
        (void)*it;
    }
}

int main() {
    shared_vec.reserve(10);  // Réserve initiale insuffisante pour déclencher réallocation

    std::thread t1(inserter);
    std::thread t2(iterator_reader);

    t1.join();
    t2.join();

    std::cout << "Vector size: " << shared_vec.size() << "\n";
    return 0;
}
