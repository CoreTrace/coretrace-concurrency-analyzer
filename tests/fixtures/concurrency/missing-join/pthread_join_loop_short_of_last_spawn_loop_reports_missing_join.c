// SPDX-License-Identifier: Apache-2.0
// Two spawn loops fill threads[0..2) and threads[2..4), but the join loop stops at 3: the thread
// in threads[3] is never joined.
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
    pthread_t threads[4];
    for (int i = 0; i < 2; i++)
        pthread_create(&threads[i], NULL, worker, NULL);
    for (int i = 2; i < 4; i++)
        pthread_create(&threads[i], NULL, worker, NULL);
    for (int i = 0; i < 3; i++)
        pthread_join(threads[i], NULL);
    return 0;
}
