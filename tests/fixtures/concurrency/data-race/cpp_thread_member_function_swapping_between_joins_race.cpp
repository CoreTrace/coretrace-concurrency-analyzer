// SPDX-License-Identifier: Apache-2.0
// The member function joins the second thread, swaps the first with a thread running idle, then
// joins the first, which waits for idle: the reader still runs when main writes (#195).
// Expected: one data race, main against reader.
#include <thread>
#include <utility>

static int shared;
static std::thread parked;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void idle() {}

struct Worker
{
    std::thread first;
    std::thread second;

    void finish();
};

void Worker::finish()
{
    second.join();
    std::swap(first, parked);
    first.join();
}

static void call(Worker& worker, void (Worker::*member)())
{
    (worker.*member)();
}

int main()
{
    parked = std::thread(idle);
    Worker worker;
    worker.second = std::thread(idle);
    worker.first = std::thread(reader);
    call(worker, &Worker::finish);
    shared = 1;
    parked.join();
    return shared;
}
