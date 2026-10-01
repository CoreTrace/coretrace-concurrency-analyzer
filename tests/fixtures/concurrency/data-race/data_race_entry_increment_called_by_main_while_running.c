// SPDX-License-Identifier: Apache-2.0
// main starts the worker thread, then calls worker() itself before the join. The entry increments
// `shared`: the race is found between its read and its write, and the write compared with itself
// must not report it a second time (#155).
// Expected: one data race on `shared`, main's call to worker() against the worker thread.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void* worker(void* argument)
{
    (void)argument;
    shared++;
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    worker(NULL);
    pthread_join(thread, NULL);
    return shared;
}
