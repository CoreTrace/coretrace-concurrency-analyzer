// SPDX-License-Identifier: Apache-2.0
// The thread publishes the address of its thread-local, and main reads through the address only on
// the branch where joining the thread failed: the thread may still run there, and its thread-local
// with it.
// Expected: no thread-local-escape, and one data race on `published`, main's read against the
// worker's publication.
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
    if (pthread_join(thread, NULL) != 0)
        return *published;
    return 0;
}
