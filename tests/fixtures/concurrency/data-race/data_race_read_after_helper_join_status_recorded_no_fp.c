// SPDX-License-Identifier: Apache-2.0
// stop() records the status of its join in a global before returning it, and main reads `shared`
// only on the branch where that status reports success: the worker has finished there. Recording
// the status does not change what stop() returns.
// Expected: no diagnostic.
#include <pthread.h>
static int shared;
static int last_status;
static pthread_t thread;
static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}
static int stop(void)
{
    return last_status = pthread_join(thread, NULL);
}
int main(void)
{
    pthread_create(&thread, NULL, worker, NULL);
    if (stop() == 0)
        return shared;
    return last_status;
}
