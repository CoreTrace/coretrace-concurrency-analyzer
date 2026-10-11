// SPDX-License-Identifier: Apache-2.0
// Hands the helper of the other unit &std::thread::detach: the reader still runs when main writes.
#include <thread>

void call(std::thread& thread, void (std::thread::*member)());

static int shared;

static void reader()
{
    int seen = shared;
    (void)seen;
}

int main()
{
    std::thread thread(reader);
    call(thread, &std::thread::detach);
    shared = 1;
    return shared;
}
