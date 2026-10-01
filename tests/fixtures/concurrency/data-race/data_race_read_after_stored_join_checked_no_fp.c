// SPDX-License-Identifier: Apache-2.0
// main keeps the join's result in a local and aborts when it reports a failure, so it reads
// `shared` only once the worker has finished. The handle is a global: no completion proof applies.
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
int main(void)
{
    pthread_create(&thread, NULL, worker, NULL);
    int status = pthread_join(thread, NULL);
    if (status != 0)
        abort();
    return shared;
}
