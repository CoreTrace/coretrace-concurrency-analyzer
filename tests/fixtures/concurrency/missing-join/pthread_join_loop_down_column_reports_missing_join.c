// SPDX-License-Identifier: Apache-2.0
// The spawn loop fills row 0 of threads, but the join loop walks column 0: it joins threads[0][0]
// and the never-filled threads[1][0], so threads[0][1] is never joined.
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
        pthread_join(threads[i][0], NULL);
    return 0;
}
