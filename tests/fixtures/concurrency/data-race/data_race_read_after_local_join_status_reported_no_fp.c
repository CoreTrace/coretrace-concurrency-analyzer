// SPDX-License-Identifier: Apache-2.0
// main tests the status of joining its local handle as it assigns it, prints it and aborts on a
// failure: past the test, the worker has finished. Printing the status does not make the join
// wait any less.
// Expected: no diagnostic.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
static int shared;
static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}
int main(void)
{
    pthread_t thread;
    int status;
    pthread_create(&thread, NULL, worker, NULL);
    if ((status = pthread_join(thread, NULL)) != 0)
    {
        fprintf(stderr, "pthread_join: %d\n", status);
        abort();
    }
    return shared;
}
