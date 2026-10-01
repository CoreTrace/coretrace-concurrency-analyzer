// SPDX-License-Identifier: Apache-2.0
// main starts the worker thread on &result, then calls worker(&result) itself before the join:
// both increment `result`. The thread's read and write are known through the object the spawn
// hands it, and only the thread makes them: main's call has accesses of its own, on the object it
// passes. The race is reported once (#155).
// Expected: one data race on `result`, main's call to worker() against the worker thread.
#include <pthread.h>
#include <stddef.h>

static int result;

static void* worker(void* argument)
{
    int* slot = argument;
    *slot += 1;
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
