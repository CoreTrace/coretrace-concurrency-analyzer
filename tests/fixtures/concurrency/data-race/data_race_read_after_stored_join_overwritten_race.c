// SPDX-License-Identifier: Apache-2.0
// main keeps the join's result in a local, then overwrites it before testing it: the test says
// nothing about the join, and when the join failed the worker may still be running while main
// reads `shared`.
// Expected: one data race on `shared`, main's read against the worker's write.
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
int main(void)
{
    pthread_create(&thread, NULL, worker, NULL);
    int status = pthread_join(thread, NULL);
    status = 0;
    if (status != 0)
        abort();
    return shared;
}
