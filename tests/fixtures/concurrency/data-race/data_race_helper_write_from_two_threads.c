// SPDX-License-Identifier: Apache-2.0
// Two different threads, `first` and `second`, both call store(), which writes `shared` with a
// single store, and neither is joined before the other starts. The two calls write `shared` at
// the same time (#155).
// Expected: one data race on `shared`, `first`'s call to store() against `second`'s.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void store(void)
{
    shared = 42;
}

static void* first(void* argument)
{
    (void)argument;
    store();
    return NULL;
}

static void* second(void* argument)
{
    (void)argument;
    store();
    return NULL;
}

int main(void)
{
    pthread_t firstThread;
    pthread_t secondThread;
    pthread_create(&firstThread, NULL, first, NULL);
    pthread_create(&secondThread, NULL, second, NULL);
    pthread_join(firstThread, NULL);
    pthread_join(secondThread, NULL);
    return shared;
}
