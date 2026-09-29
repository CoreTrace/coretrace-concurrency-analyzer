// SPDX-License-Identifier: Apache-2.0
// stop() logs a failed join and returns 0 anyway, so main, which reads `shared` when stop()
// returns 0, also reads it when the join failed and the worker may still be running.
// Expected: one data race on `shared`, main's read against the worker's write.
#include <pthread.h>
#include <stdio.h>
static int shared;
static pthread_t thread;
static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}
static int stop(void)
{
    int status = pthread_join(thread, NULL);
    if (status != 0)
    {
        fprintf(stderr, "pthread_join: %d\n", status);
        status = 0;
    }
    return status;
}
int main(void)
{
    pthread_create(&thread, NULL, worker, NULL);
    if (stop() == 0)
        return shared;
    return 0;
}
