// SPDX-License-Identifier: Apache-2.0
// Threads are created into threads[0][0], threads[0][1] and threads[1][0], and a loop joins column 0:
// threads[0][1] lies between the column's handles, and its thread is never joined.
// Expected: one missing join.
#include <pthread.h>
#include <stddef.h>

static void* other(void* argument)
{
    (void)argument;
    return NULL;
}

int main(void)
{
    pthread_t threads[2][2];
    pthread_create(&threads[0][0], NULL, other, NULL);
    pthread_create(&threads[0][1], NULL, other, NULL);
    pthread_create(&threads[1][0], NULL, other, NULL);
    for (int i = 0; i < 2; i++)
        pthread_join(threads[i][0], NULL);
    return 0;
}
