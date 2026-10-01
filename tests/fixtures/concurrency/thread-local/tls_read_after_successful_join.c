// SPDX-License-Identifier: Apache-2.0
// The thread publishes the address of its thread-local, and main reads through the address on the
// branch where joining the thread succeeded: the thread-local ended with its thread there.
// Expected: one thread-local-escape.
#include <pthread.h>
#include <stddef.h>

static _Thread_local int slot;
static int* published;

static void* worker(void* argument)
{
    (void)argument;
    slot = 1;
    published = &slot;
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    if (pthread_join(thread, NULL) == 0)
        return *published;
    return 0;
}
