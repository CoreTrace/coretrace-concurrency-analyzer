// SPDX-License-Identifier: Apache-2.0
// finish() aborts when the join stop() makes fails, so it returns only once the worker has
// finished, and main then reads `shared`.
// Expected: no diagnostic.
#include <pthread.h>
#include <stdlib.h>
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
        abort();
}
int main(void)
{
    pthread_create(&thread, NULL, worker, NULL);
    finish();
    return shared;
}
