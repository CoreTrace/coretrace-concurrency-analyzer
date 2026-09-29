// SPDX-License-Identifier: Apache-2.0
// stop() returns whether its join succeeded, not the join's status: zero now means a failure, and
// main reads `shared` exactly then, while the worker may still be running.
// Expected: one data race on `shared`, main's read against the worker's write.
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
    return !pthread_join(thread, NULL);
}
int main(void)
{
    pthread_create(&thread, NULL, worker, NULL);
    if (stop() == 0)
        return shared;
    return 0;
}
