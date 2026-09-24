// SPDX-License-Identifier: Apache-2.0
// A thread publishes the address of its thread-local value and waits: main reads through it
// while the owner is still running, then lets it finish. The object is alive whenever it is read.
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>

static _Thread_local int hits = 0;
static int* _Atomic published = NULL;
static atomic_int done = 0;

static void* worker(void* argument)
{
    (void)argument;
    hits = 5;
    atomic_store(&published, &hits);
    while (!atomic_load(&done))
        ;
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    int* observed;
    while ((observed = atomic_load(&published)) == NULL)
        ;
    int seen = *observed;
    atomic_store(&done, 1);
    pthread_join(thread, NULL);
    return seen;
}
