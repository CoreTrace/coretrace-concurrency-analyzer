// SPDX-License-Identifier: Apache-2.0
// stop() logs the status of its join before returning it, and main reads `shared` only on the
// branch where that status reports success: the worker has finished there. Logging the status
// does not change what stop() returns.
// Expected: no diagnostic.
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
    printf("pthread_join: %d\n", status);
    return status;
}
int main(void)
{
    pthread_create(&thread, NULL, worker, NULL);
    if (stop() == 0)
        return shared;
    return 0;
}
