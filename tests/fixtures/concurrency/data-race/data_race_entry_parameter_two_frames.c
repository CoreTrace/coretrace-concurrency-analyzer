// SPDX-License-Identifier: Apache-2.0
// The entry reaches the spawn through two helpers, each handing it on as a parameter: main writes
// while the thread runs, before joining it (#123).
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

static void begin(void* (*entry)(void*))
{
    pthread_create(&thread, NULL, entry, NULL);
}

static void start(void* (*entry)(void*))
{
    begin(entry);
}

int main(void)
{
    start(worker);
    shared += 1;
    pthread_join(thread, NULL);
    return shared;
}
