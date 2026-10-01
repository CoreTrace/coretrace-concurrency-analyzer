// SPDX-License-Identifier: Apache-2.0
// Two spawn loops fill threads[0..2) and threads[2..4), and the loop joining all four may leave
// early: the threads after the break are never joined.
// Expected: one missing join.
#include <pthread.h>
#include <stddef.h>

extern int stop;

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
    for (int i = 0; i < 4; i++)
    {
        pthread_join(threads[i], NULL);
        if (stop)
            break;
    }
    return 0;
}
