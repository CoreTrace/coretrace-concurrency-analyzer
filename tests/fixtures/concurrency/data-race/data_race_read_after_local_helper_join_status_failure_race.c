// SPDX-License-Identifier: Apache-2.0
// stop() joins the handle it is given and returns the status, and main reads `shared` on the
// branch where that status reports a failure: the worker may still run there.
// Expected: one data race on `shared`, main's read against the worker's write.
#include <pthread.h>
static int shared;
static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}
static int stop(pthread_t thread)
{
    return pthread_join(thread, NULL);
}
int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    if (stop(thread) != 0)
        return shared;
    return 0;
}
