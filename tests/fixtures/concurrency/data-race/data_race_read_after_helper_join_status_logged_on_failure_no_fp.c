// SPDX-License-Identifier: Apache-2.0
// stop() keeps the status of its join, logs it only when it reports a failure, and returns it:
// main reads `shared` only on the branch where that status reports success, and the worker has
// finished there.
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
    if (status != 0)
        fprintf(stderr, "pthread_join: %d\n", status);
    return status;
}
int main(void)
{
    pthread_create(&thread, NULL, worker, NULL);
    if (stop() == 0)
        return shared;
    return 0;
}
