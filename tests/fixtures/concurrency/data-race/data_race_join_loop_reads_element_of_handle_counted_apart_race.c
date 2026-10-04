// SPDX-License-Identifier: Apache-2.0
// Handles are stored at threads[started], raised on success only, and thread i writes
// results[i]: once a creation fails, the thread joined at slot k is not the one writing
// results[k], which may still run when the join loop reads it (#108).
// Expected: one data race.
#include <pthread.h>
static int results[4];
static void* worker(void* argument)
{
    int* slot = argument;
    *slot = 42;
    return NULL;
}
int main(void)
{
    pthread_t threads[4];
    int total = 0;
    int started = 0;
    for (int i = 0; i < 4; ++i)
        if (pthread_create(&threads[started], NULL, worker, &results[i]) == 0)
            ++started;
    for (int k = 0; k < started; ++k)
    {
        pthread_join(threads[k], NULL);
        total += results[k];
    }
    return total;
}
