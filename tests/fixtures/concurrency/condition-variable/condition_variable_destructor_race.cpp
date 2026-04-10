// SPDX-License-Identifier: Apache-2.0
// Test: Condition variable - destruction pendant qu'un thread est bloqué dans wait()
//
// La norme C++ exige que tous les threads aient quitté wait() avant que la
// destruction d'un std::condition_variable soit sûre. Détruire la cv pendant
// qu'un thread y est bloqué est un comportement indéfini (précondition violée).
// Pattern typique : objet propriétaire de la cv détruit par un thread pendant
// qu'un autre attend une notification qui ne viendra jamais.
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

std::mutex              mtx;
std::condition_variable* cv  = nullptr;
bool                    ready = false;

void waiter() {
    std::unique_lock<std::mutex> lock(mtx);
    // Attend une notification — ne recevra jamais de notify car
    // le thread principal détruit cv avant de l'appeler.
    cv->wait(lock, [] { return ready; });  // UB: cv peut être détruit pendant ce wait
    std::cout << "waiter unblocked\n";
}

int main() {
    cv = new std::condition_variable();

    std::thread t(waiter);
    // Laisse le waiter entrer dans wait()
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // CONDITION VARIABLE: détruit cv sans avoir appelé notify — le thread
    // waiter est encore bloqué dans cv->wait() sur l'objet que l'on supprime.
    delete cv;  // UB: destruction d'une cv avec un waiter actif
    cv = nullptr;

    // Le waiter est bloqué sur une cv détruite : join() ne retournera jamais
    // (ou crash selon l'implémentation). C'est le bug.
    t.detach();  // Évite un join() bloquant dans ce test — le bug est le delete ci-dessus
    return 0;
}
