// SPDX-License-Identifier: Apache-2.0
// Each worker is handed the holder whose array keeps its handle, and rewrites a handle main joins
// with that of an idle thread: main may join idle instead of a worker, which still runs when main
// writes. The holder's destructor is handed the local too (#206).
// Expected: two data races, main's join against the worker rewriting the handle and main's write
// against the worker's read; the idle thread and the workers are not proven joined, and the
// holder's address reaches the workers.
#include <pthread.h>

static int shared;

struct Holder
{
    pthread_t threads[2];
    pthread_t parked;

    ~Holder() {}
};

static void* idle(void*)
{
    return nullptr;
}

static void* worker(void* argument)
{
    Holder* holder = static_cast<Holder*>(argument);
    holder->threads[1] = holder->parked;
    int seen = shared;
    (void)seen;
    return nullptr;
}

int main()
{
    Holder holder;
    pthread_create(&holder.parked, nullptr, idle, nullptr);
    for (int i = 0; i < 2; ++i)
        pthread_create(&holder.threads[i], nullptr, worker, &holder);
    for (int i = 0; i < 2; ++i)
        pthread_join(holder.threads[i], nullptr);
    shared = 1;
    return shared;
}
