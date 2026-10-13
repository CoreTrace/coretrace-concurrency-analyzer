// SPDX-License-Identifier: Apache-2.0
// The helper starts a thread on the entry it is handed and joins it before returning: main's write
// after the call runs beside nothing (#123).
// Expected: no diagnostic.
#include <pthread.h>

static int shared;

static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}

static void run(void* (*entry)(void*))
{
    pthread_t thread;
    pthread_create(&thread, NULL, entry, NULL);
    pthread_join(thread, NULL);
}

int main(void)
{
    run(worker);
    shared += 1;
    return shared;
}
