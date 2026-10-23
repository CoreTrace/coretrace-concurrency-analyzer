// SPDX-License-Identifier: Apache-2.0
// A loop calls, through a pointer-to-member, a member function joining each object's second
// thread, running idle; the readers the first threads hold still run when main writes. The join
// lies 16 bytes into each object: a proof reading it at the object's own address, as the loop
// proofs read a site's operand, would take it for a join of the first threads and hide the race.
// Only the match of a creation to its join outside a loop reads such a join (#195).
// Expected: one data race, main against reader.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void idle() {}

struct Worker
{
    std::thread first;
    long tag = 0;
    std::thread second;

    void finish()
    {
        second.join();
    }
};

static void call(Worker& worker, void (Worker::*member)())
{
    (worker.*member)();
}

int main()
{
    Worker workers[2];
    workers[0].second = std::thread(idle);
    workers[1].second = std::thread(idle);
    for (int i = 0; i < 2; ++i)
        workers[i].first = std::thread(reader);
    for (int i = 0; i < 2; ++i)
        call(workers[i], &Worker::finish);
    shared = 1;
    for (int i = 0; i < 2; ++i)
        workers[i].first.join();
    return shared;
}
