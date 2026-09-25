// SPDX-License-Identifier: Apache-2.0
// The thread publishes the address of its thread-local, a helper joins the thread, and main then
// reads through the address: the thread-local ended with its thread.
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

static void join_worker(pthread_t thread)
{
    pthread_join(thread, NULL);
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    join_worker(thread);
    return *published;
}
