// SPDX-License-Identifier: Apache-2.0
// main and the worker thread both call bump(), which increments `shared`, and main calls it
// before the join. The increment reads and writes `shared`: the race is found between those two
// accesses, and the write compared with itself must not report it a second time (#155).
// Expected: one data race on `shared`, main's call to bump() against the worker thread's.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void bump(void)
{
    shared++;
}

static void* worker(void* argument)
{
    (void)argument;
    bump();
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    bump();
    pthread_join(thread, NULL);
    return shared;
}
