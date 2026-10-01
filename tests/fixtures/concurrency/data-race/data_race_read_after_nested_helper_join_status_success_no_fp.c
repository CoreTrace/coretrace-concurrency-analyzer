// SPDX-License-Identifier: Apache-2.0
// finish() returns what stop() returns, the status of its join, and main reads `shared` only on the
// branch where that status reports success: the worker has finished there.
// Expected: no diagnostic.
#include <pthread.h>
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
    return pthread_join(thread, NULL);
}
static int finish(void)
{
    return stop();
}
int main(void)
{
    pthread_create(&thread, NULL, worker, NULL);
    if (finish() == 0)
        return shared;
    return 0;
}
