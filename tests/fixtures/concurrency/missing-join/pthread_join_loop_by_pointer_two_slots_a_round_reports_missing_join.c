// SPDX-License-Identifier: Apache-2.0
// A spawn loop fills threads[0..4), but the pointer loop moves two handles a round: it joins
// threads[0] and threads[2] only, and the threads in threads[1] and threads[3] still run when
// main writes.
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

int main(void)
{
    pthread_t threads[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&threads[i], NULL, worker, NULL);
    for (pthread_t* handle = threads; handle != threads + 4; handle += 2)
        pthread_join(*handle, NULL);
    shared += 2;
    return shared;
}
