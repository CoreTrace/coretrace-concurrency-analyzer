// SPDX-License-Identifier: Apache-2.0
// The `outer` thread starts `inner`, then calls store() before joining it, and `inner` calls
// store() too. store() writes `shared` with a single store, so the two threads write it at the
// same time (#155).
// Expected: one data race on `shared`, `outer`'s call to store() against `inner`'s.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void store(void)
{
    shared = 42;
}

static void* inner(void* argument)
{
    (void)argument;
    store();
    return NULL;
}

static void* outer(void* argument)
{
    (void)argument;
    pthread_t thread;
    pthread_create(&thread, NULL, inner, NULL);
    store();
    pthread_join(thread, NULL);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, outer, NULL);
    pthread_join(thread, NULL);
    return shared;
}
