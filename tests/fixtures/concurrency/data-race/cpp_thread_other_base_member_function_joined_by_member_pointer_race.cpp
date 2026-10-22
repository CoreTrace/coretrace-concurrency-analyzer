// SPDX-License-Identifier: Apache-2.0
// The pointer-to-member names the second base's member function, which joins that base's idle
// thread: the reader the first base holds still runs when main writes (#195).
// Expected: one data race, main against reader.
#include <thread>

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

static void idle() {}

struct First
{
    std::thread thread;

    void finish()
    {
        thread.join();
    }
};

struct Second
{
    std::thread thread;

    void finish()
    {
        thread.join();
    }
};

struct Both : First, Second
{
};

static void call(Both& both, void (Both::*member)())
{
    (both.*member)();
}

int main()
{
    Both both;
    both.First::thread = std::thread(reader);
    both.Second::thread = std::thread(idle);
    call(both, &Second::finish);
    shared = 1;
    both.First::thread.join();
    return shared;
}
