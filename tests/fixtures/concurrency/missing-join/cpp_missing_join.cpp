// SPDX-License-Identifier: Apache-2.0
// Test: Missing join C++ - threads std::thread non joints
#include <thread>
#include <iostream>
#include <vector>

int shared_sum = 0;

void worker(int id)
{
    for (int i = 0; i < 100; i++)
    {
        shared_sum += id;
    }
    std::cout << "Thread " << id << " finished\n";
}

int main(void)
{
    std::vector<std::thread> threads;

    // Création de threads
    for (int i = 0; i < 5; i++)
    {
        threads.emplace_back(worker, i);
    }

    // MISSING JOIN: aucun thread n'est joint!
    // En C++, cela appelle std::terminate() si le thread est still joinable
    // Commentaire pour éviter la termination réelle pendant le test:
    // for (auto& t : threads) { t.join(); }

    std::cout << "Main exiting, sum = " << shared_sum << "\n";

    // Nettoyage manuel pour éviter terminate() pendant le test
    for (auto& t : threads)
    {
        if (t.joinable())
        {
            t.detach();  // Ou mieux: t.join()
        }
    }

    return 0;
}
