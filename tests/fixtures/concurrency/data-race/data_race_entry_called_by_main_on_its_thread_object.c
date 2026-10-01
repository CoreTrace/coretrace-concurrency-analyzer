// SPDX-License-Identifier: Apache-2.0
// main starts the worker thread on &result, then calls worker(&result) itself before the join:
// the call and the thread both write `result`. The thread's write is known through the object
// the spawn hands it, and the call's through its own argument; neither stands for the other, so
// the race is reported once (#155).
// Expected: one data race on `result`, main's call to worker() against the worker thread.
#include <pthread.h>
#include <stddef.h>

static int result;

static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, &result);
    worker(&result);
    pthread_join(thread, NULL);
    return result;
}
