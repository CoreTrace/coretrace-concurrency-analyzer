// SPDX-License-Identifier: Apache-2.0
// stop() returns the status of joining the handle it is given, and run_worker() returns when that
// status reports a failure: the worker may then still use the local of the frame that is gone.
// Expected: one thread-arg-escape, at the creation.
#include <pthread.h>
static void* worker(void* argument)
{
    int* value = (int*)argument;
    *value += 1;
    return NULL;
}
static int stop(pthread_t thread)
{
    return pthread_join(thread, NULL);
}
void run_worker(void)
{
    int local_value = 42;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, &local_value);
    if (stop(thread) != 0)
        return;
}
