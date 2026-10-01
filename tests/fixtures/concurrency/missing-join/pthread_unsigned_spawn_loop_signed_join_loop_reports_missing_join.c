// SPDX-License-Identifier: Apache-2.0
// Both loops run to the same bound n, but the spawn loop compares unsigned and the join loop
// signed: with n negative, the spawn loop starts threads the join loop never reaches.
// Expected: one missing join.
#include <pthread.h>
#include <stddef.h>

static void* worker(void* argument)
{
    (void)argument;
    return NULL;
}

int main(int argc, char** argv)
{
    (void)argv;
    pthread_t threads[64];
    int n = argc - 2;
    for (unsigned i = 0; i < (unsigned)n; i++)
        pthread_create(&threads[i], NULL, worker, NULL);
    for (int i = 0; i < n; i++)
        pthread_join(threads[i], NULL);
    return 0;
}
