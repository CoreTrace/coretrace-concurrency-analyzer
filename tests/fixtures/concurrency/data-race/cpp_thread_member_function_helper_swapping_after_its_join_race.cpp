// SPDX-License-Identifier: Apache-2.0
// The member function hands its object to a helper that joins the second thread, then swaps the
// first, running the reader, with a thread running idle; the member function's own join of the
// first thread then waits for idle. A swap past the helper's join still precedes that join: the
// reader still runs when main writes (#195).
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

static void joinSecond(Worker& worker)
{
    worker.second.join();
    std::swap(worker.first, parked);
}

void Worker::finish()
{
    joinSecond(*this);
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
