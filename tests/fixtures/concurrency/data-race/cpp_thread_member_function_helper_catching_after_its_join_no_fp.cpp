// SPDX-License-Identifier: Apache-2.0
// As cpp_thread_member_function_helper_swapping_in_catch_after_its_join_race.cpp, the catch
// leaving the object alone: the member function's join ends the reader before main writes (#195).
// Expected: no diagnostic.
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
