// SPDX-License-Identifier: Apache-2.0
// As cpp_thread_member_function_helper_swapping_after_its_join_race.cpp, the helper swapping the
// first thread before it joins the second (#195).
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
    std::swap(worker.first, parked);
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
