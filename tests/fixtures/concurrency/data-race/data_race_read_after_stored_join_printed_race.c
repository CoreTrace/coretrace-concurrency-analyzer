// SPDX-License-Identifier: Apache-2.0
// main prints the join's result but goes on whatever it says: when the join failed, the worker may
// still be running while main reads `shared`.
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
int main(void)
{
    pthread_create(&thread, NULL, worker, NULL);
    int status = pthread_join(thread, NULL);
    printf("%d\n", status);
    return shared;
}
