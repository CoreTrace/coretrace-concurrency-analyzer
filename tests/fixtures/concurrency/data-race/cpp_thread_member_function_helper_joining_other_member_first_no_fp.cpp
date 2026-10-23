// SPDX-License-Identifier: Apache-2.0
// The member function hands its object to a helper joining the second thread only, then joins the
// first: both are joined before main writes (#195).
// Expected: no diagnostic.
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
