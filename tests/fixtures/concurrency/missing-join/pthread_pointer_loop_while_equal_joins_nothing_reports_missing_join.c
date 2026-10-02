// SPDX-License-Identifier: Apache-2.0
// The pointer loop goes on while the cursor equals threads + 2, which it never does: it joins
// nothing, and both threads still run when main writes.
// Expected: one data race and two missing joins.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void* other(void* argument)
{
    (void)argument;
    return NULL;
}

static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}

int main(void)
{
    pthread_t threads[2];
    pthread_create(&threads[0], NULL, worker, NULL);
    pthread_create(&threads[1], NULL, other, NULL);
    for (pthread_t* handle = threads; handle == threads + 2; ++handle)
        pthread_join(*handle, NULL);
    shared += 2;
    return shared;
}
