// SPDX-License-Identifier: Apache-2.0
// The join loop may break after joining the idle thread, before the writer: main's write after the
// loop then runs beside the writer; the program then exits (#162).
// Expected: one data race, main's write against the writer's.
#include <cstdlib>
#include <thread>
#include <vector>

static int shared;

static void writer()
{
    shared += 1;
}

static void idle() {}

int main(int argc, char**)
{
    std::vector<std::thread> threads;
    threads.push_back(std::thread(idle));
    threads.push_back(std::thread(writer));
    for (auto& thread : threads)
    {
        thread.join();
        if (argc > 1)
            break;
    }
    shared += 2;
    std::exit(0);
}
