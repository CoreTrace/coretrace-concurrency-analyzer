// SPDX-License-Identifier: Apache-2.0
// finish() only reports a failure of the join stop() makes, and returns either way: after it,
// the worker may still run while main reads `shared`.
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
    return pthread_join(thread, NULL);
}
static void finish(void)
{
    if (stop() != 0)
        perror("pthread_join");
}
int main(void)
{
    pthread_create(&thread, NULL, worker, NULL);
    finish();
    return shared;
}
