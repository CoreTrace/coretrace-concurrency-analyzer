// SPDX-License-Identifier: Apache-2.0
// main and the worker thread both call load(), which only reads `shared`: two reads never race
// (#155).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int shared = 1;

static int load(void)
{
    return shared;
}

static void* worker(void* argument)
{
    (void)argument;
    return (void*)(long)load();
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    int value = load();
    pthread_join(thread, NULL);
    return value;
}
