// SPDX-License-Identifier: Apache-2.0
// Two spawn loops fill the two rows of threads, but the join loop walks row 0 only: the threads in
// row 1 are never joined. The rows differ only by a constant offset of the same array.
// Expected: one missing join.
#include <pthread.h>
#include <stddef.h>

static void* worker(void* argument)
{
    (void)argument;
    return NULL;
}

int main(void)
{
    pthread_t threads[2][2];
    for (int i = 0; i < 2; i++)
        pthread_create(&threads[0][i], NULL, worker, NULL);
    for (int i = 0; i < 2; i++)
        pthread_create(&threads[1][i], NULL, worker, NULL);
    for (int i = 0; i < 2; i++)
        pthread_join(threads[0][i], NULL);
    return 0;
}
