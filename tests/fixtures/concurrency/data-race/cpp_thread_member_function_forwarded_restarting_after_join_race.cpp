// SPDX-License-Identifier: Apache-2.0
// As cpp_thread_member_function_restarting_other_after_join_race.cpp, the pointer-to-member
// forwarded to the wrapper calling it by another function (#195).
// The analysis reports the race against reader, which the call joined: it does not track the
// thread a call through a pointer-to-member starts (#222), and only refuses to take that join
// as known. The race being reported does not show the new thread is tracked.
// Expected: one data race on shared, reported as main against reader.
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

struct Worker
{
    std::thread thread;

    void finish()
    {
        thread.join();
        thread = std::thread(other);
    }
};

static void call(Worker& worker, void (Worker::*member)())
{
    (worker.*member)();
}

static void forward(Worker& worker, void (Worker::*member)())
{
    call(worker, member);
}

int main()
{
    Worker worker{std::thread(reader)};
    forward(worker, &Worker::finish);
    shared = 1;
    worker.thread.join();
    return shared;
}
