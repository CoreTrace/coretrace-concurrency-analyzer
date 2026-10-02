// SPDX-License-Identifier: Apache-2.0
// The spawn loops fill threads[0..2) and threads[2..4), and one loop joins all four; but the
// second loop hands the array to its threads, and they overwrite threads[0] before main joins it:
// the first thread is never joined.
// Expected: one missing join.
#include <pthread.h>
#include <stddef.h>

static void* worker(void* argument)
{
    (void)argument;
    return NULL;
}

static void* clearer(void* argument)
{
    pthread_t* handles = argument;
    handles[0] = handles[3];
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    for (int i = 0; i < 2; i++)
        pthread_create(&threads[i], NULL, worker, NULL);
    for (int i = 2; i < 4; i++)
        pthread_create(&threads[i], NULL, clearer, threads);
    for (int i = 0; i < 4; i++)
        pthread_join(threads[i], NULL);
    return 0;
}
