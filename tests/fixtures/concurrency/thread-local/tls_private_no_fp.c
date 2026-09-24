// SPDX-License-Identifier: Apache-2.0
// Every thread updates its own copy of a thread-local counter: nothing is shared, and nothing
// outlives its thread.
#include <pthread.h>
#include <stddef.h>

static _Thread_local int counter = 0;

static void* worker(void* argument)
{
    (void)argument;
    for (int i = 0; i < 100; i++)
        counter++;
    return (void*)(long)counter;
}

int main(void)
{
    pthread_t threads[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&threads[i], NULL, worker, NULL);
    for (int i = 0; i < 4; i++)
        pthread_join(threads[i], NULL);
    return 0;
}
