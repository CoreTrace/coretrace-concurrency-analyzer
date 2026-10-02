// SPDX-License-Identifier: Apache-2.0
// main hands the handle array, read back from a local, to a thread that overwrites threads[0] with
// threads[1]: the first worker is never joined, and runs when main writes.
// Expected: one data race and one missing join.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}

static void* overwrite_first(void* argument)
{
    pthread_t* handles = argument;
    handles[0] = handles[1];
    return NULL;
}

int main(void)
{
    pthread_t threads[2];
    for (int i = 0; i < 2; i++)
        pthread_create(&threads[i], NULL, worker, NULL);
    pthread_t* saved = threads;
    pthread_t helper;
    pthread_create(&helper, NULL, overwrite_first, saved);
    pthread_join(helper, NULL);
    for (int i = 0; i < 2; i++)
        pthread_join(threads[i], NULL);
    shared += 2;
    return shared;
}
