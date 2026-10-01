// SPDX-License-Identifier: Apache-2.0
// Two different threads both call load(), which only reads `shared`: two reads never race
// (#155).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int shared = 1;

static int load(void)
{
    return shared;
}

static void* first(void* argument)
{
    (void)argument;
    return (void*)(long)load();
}

static void* second(void* argument)
{
    (void)argument;
    return (void*)(long)load();
}

int main(void)
{
    pthread_t firstThread;
    pthread_t secondThread;
    pthread_create(&firstThread, NULL, first, NULL);
    pthread_create(&secondThread, NULL, second, NULL);
    pthread_join(firstThread, NULL);
    pthread_join(secondThread, NULL);
    return 0;
}
