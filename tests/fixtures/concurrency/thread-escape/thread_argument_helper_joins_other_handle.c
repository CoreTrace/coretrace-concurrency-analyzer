// SPDX-License-Identifier: Apache-2.0
// The helper joins its second handle, and the thread holding the local is passed as the first:
// joining some thread is not joining this one. The reader is detached, so the frame is the only
// question left.
#include <pthread.h>
#include <stddef.h>

static void* worker(void* argument)
{
    int* value = (int*)argument;
    *value += 1;
    return NULL;
}

static void* idle(void* argument)
{
    (void)argument;
    return NULL;
}

static void join_second(pthread_t first, pthread_t second)
{
    (void)first;
    pthread_join(second, NULL);
}

void run_workers(void)
{
    int local_value = 42;
    pthread_t reader;
    pthread_t other;

    pthread_create(&reader, NULL, worker, &local_value);
    pthread_create(&other, NULL, idle, NULL);
    join_second(reader, other);
    pthread_detach(reader);
}
