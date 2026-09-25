// SPDX-License-Identifier: Apache-2.0
// main reads the result after a helper has joined the thread that wrote it: the join orders the
// write before the read, as a join in place would.
#include <pthread.h>
#include <stddef.h>

static int result;

static void* worker(void* argument)
{
    (void)argument;
    result = 42;
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
    return result;
}
