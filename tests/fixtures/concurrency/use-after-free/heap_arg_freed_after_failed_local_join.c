// SPDX-License-Identifier: Apache-2.0
// The same with the handle in a local: when pthread_join fails, the worker may still read `data`,
// so the free on that branch comes too early. The free after a successful join is right.
// Expected: one thread-arg-freed, at the first free.
#include <pthread.h>
#include <stdlib.h>
static int sink;
static void* worker(void* argument)
{
    sink = *(int*)argument;
    return NULL;
}
int main(void)
{
    int* data = malloc(sizeof *data);
    *data = 1;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, data);
    if (pthread_join(thread, NULL) != 0)
    {
        free(data);
        return 1;
    }
    free(data);
    return sink;
}
