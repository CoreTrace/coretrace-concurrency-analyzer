// SPDX-License-Identifier: Apache-2.0
// The helper joins the second thread, then the call it makes throws; its catch swaps the first
// thread, running the reader, with one running idle, and the helper returns normally. The member
// function's join of the first thread then waits for idle: the reader still runs when main
// writes. A change on an exception path past a helper's join is no less one (#195).
// Expected: one data race, main against reader.
#include <thread>
#include <utility>

static int shared;
static int caught;
static std::thread parked;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void idle() {}

static void interrupt()
{
    throw 0;
}

struct Worker
{
    std::thread first;
    std::thread second;

    void finish();
};

static void joinSecond(Worker& worker)
{
    worker.second.join();
    try
    {
        interrupt();
    }
    catch (...)
    {
        ++caught;
        std::swap(worker.first, parked);
    }
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
    return shared + caught;
}
