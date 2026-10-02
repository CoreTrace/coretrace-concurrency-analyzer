// SPDX-License-Identifier: Apache-2.0
// The local's thread is swapped with another one before the local is moved into the vector: the
// loop joins the idle thread, and the writer, now in the other local, still runs when main writes
// after the loop (#162).
// Expected: one data race, main's write against the writer's.
#include <thread>
#include <utility>
#include <vector>

static int shared;

static void writer()
{
    shared += 1;
}

static void idle() {}

int main()
{
    std::vector<std::thread> threads;
    std::thread thread(writer);
    std::thread other(idle);
    thread.swap(other);
    threads.push_back(std::move(thread));
    for (auto& joined : threads)
        joined.join();
    shared += 2;
    other.join();
    return shared;
}
