// SPDX-License-Identifier: Apache-2.0
// As cpp_thread_member_function_restarting_through_function_pointer_race.cpp, the function the
// local points to starting no thread: what it does is established, and the member function's
// join ends the reader before main writes (#195).
// Expected: no diagnostic.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void other()
{
    int seen = shared;
    (void)seen;
}

static int noted;

static void note(std::thread& thread)
{
    (void)thread;
    ++noted;
}

struct Worker
{
    std::thread thread;

    void finish()
    {
        thread.join();
        void (*after)(std::thread&) = note;
        after(thread);
    }
};

static void call(Worker& worker, void (Worker::*member)())
{
    (worker.*member)();
}

int main()
{
    Worker worker{std::thread(reader)};
    call(worker, &Worker::finish);
    shared = 1;
    worker.thread.join();
    return shared;
}
