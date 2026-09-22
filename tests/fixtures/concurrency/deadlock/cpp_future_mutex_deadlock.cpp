// SPDX-License-Identifier: Apache-2.0
// Test: C++ - Deadlock entre std::future::get() et une tâche async attendant le même mutex
//
// Le thread principal acquiert un mutex puis appelle future.get(), bloquant
// jusqu'à ce que la tâche asynchrone produise sa valeur.
// La tâche async tente d'acquérir ce même mutex avant de produire la valeur :
// dépendance circulaire — deadlock garanti.
// Pattern fréquent quand on ajoute un verrou dans une callback sans vérifier
// les dépendances avec le code qui lance la tâche.
#include <future>
#include <iostream>
#include <mutex>

std::mutex mtx;
int shared_value = 10;

int compute() {
    // DEADLOCK: cette tâche doit acquérir mtx, mais l'appelant tient mtx
    // et est bloqué sur get() en attendant que compute() retourne.
    std::lock_guard<std::mutex> lock(mtx);  // DEADLOCK: attend un verrou déjà tenu
    return shared_value * 2;
}

int main() {
    std::future<int> f = std::async(std::launch::async, compute);

    {
        std::lock_guard<std::mutex> lock(mtx);  // Acquiert le verrou
        // DEADLOCK: get() bloque ici indéfiniment car compute() attend mtx,
        // que ce thread ne libérera qu'après le retour de get().
        int result = f.get();
        std::cout << "result = " << result << "\n";
    }
    return 0;
}
