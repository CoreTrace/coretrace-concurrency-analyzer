// SPDX-License-Identifier: Apache-2.0
// emplace_back builds each thread in the vector from the entry it is handed: main writes between
// the loop starting them and the loop joining them, and the workers write together (#123).
// Expected: two data races, main against worker and worker against worker.
#include <thread>
#include <vector>

static int shared;

static void worker()
{
    shared += 1;
}

int main()
{
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; ++i)
        threads.emplace_back(worker);
    shared += 1;
    for (auto& thread : threads)
        thread.join();
    shared += 2;
    return shared;
}
