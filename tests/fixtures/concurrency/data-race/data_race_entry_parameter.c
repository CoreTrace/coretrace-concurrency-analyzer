// SPDX-License-Identifier: Apache-2.0
// The helper starts a thread on the entry its caller hands it as a parameter: main writes while
// that thread runs, before joining it (#123).
// Expected: one data race, main against worker.
#include <pthread.h>

static int shared;
static pthread_t thread;

static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}

static void start(void* (*entry)(void*))
{
    pthread_create(&thread, NULL, entry, NULL);
}

int main(void)
{
    start(worker);
    shared += 1;
    pthread_join(thread, NULL);
    return shared;
}
