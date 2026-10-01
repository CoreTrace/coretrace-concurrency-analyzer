// SPDX-License-Identifier: Apache-2.0
// main and the worker thread both call store(), which writes `shared`, but main calls it only
// after joining the thread: the join orders the thread's write before main's (#155).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void store(void)
{
    shared = 42;
}

static void* worker(void* argument)
{
    (void)argument;
    store();
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    pthread_join(thread, NULL);
    store();
    return shared;
}
