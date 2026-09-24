// SPDX-License-Identifier: Apache-2.0
// The shared pointer receives a thread-local address, but also a heap one: after the join it may
// hold either, so a read through it is not known to reach a dead thread's object.
#include <pthread.h>
#include <stdlib.h>

static _Thread_local int local_value = 0;
static int* shared = NULL;

static void* worker(void* argument)
{
    (void)argument;
    local_value = 7;
    shared = &local_value;
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    pthread_join(thread, NULL);
    shared = malloc(sizeof *shared);
    *shared = 1;
    int value = *shared;
    free(shared);
    return value;
}
