// SPDX-License-Identifier: Apache-2.0
// The count is raised when a creation fails, not when it succeeds: the next creation stores over
// the handle of the thread just started, which is lost.
// Expected: one missing join.
#include <pthread.h>

static void* worker(void* argument)
{
    (void)argument;
    return NULL;
}

int main(void)
{
    pthread_t threads[4];
    int started = 0;
    for (int i = 0; i < 4; ++i)
    {
        if (pthread_create(&threads[started], NULL, worker, NULL) != 0)
            ++started;
    }
    for (int k = 0; k < started; ++k)
        pthread_join(threads[k], NULL);
    return started;
}
