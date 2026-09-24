// SPDX-License-Identifier: Apache-2.0
// The thread is handed a pointer to a local and detached: the frame is gone as soon as the
// function returns, while the thread may still read through the pointer.
#include <thread>

static void read_value(const int* value)
{
    volatile int copy = *value;
    (void)copy;
}

static void start_detached()
{
    int local = 42;
    std::thread worker(read_value, &local);
    worker.detach();
}

int main()
{
    start_detached();
    return 0;
}
