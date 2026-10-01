// SPDX-License-Identifier: Apache-2.0
// The thread publishes the address of its thread-local, each branch of the `if` joins it, and main
// then reads through the address: the thread-local ended with its thread on either branch.
// Expected: one thread-local-escape. The missing join (#143) and the race on `published` (#113)
// are reported elsewhere.
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

int main(int argc, char** argv)
{
    (void)argv;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    if (argc > 1)
        pthread_join(thread, NULL);
    else
        pthread_join(thread, NULL);
    return *published;
}
