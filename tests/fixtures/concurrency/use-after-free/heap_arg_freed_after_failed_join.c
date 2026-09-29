// SPDX-License-Identifier: Apache-2.0
// When pthread_join fails, the worker may still read `data`: the free on that branch comes too
// early. The free after a successful join is right. The handle is a global, so no completion
// proof applies and only the rule's own join test decides.
// Expected: one thread-arg-freed, at the first free.
#include <pthread.h>
#include <stdlib.h>
static int sink;
static pthread_t thread;
static void* worker(void* argument)
{
    sink = *(int*)argument;
    return NULL;
}
int main(void)
{
    int* data = malloc(sizeof *data);
    *data = 1;
    pthread_create(&thread, NULL, worker, data);
    if (pthread_join(thread, NULL) != 0)
    {
        free(data);
        return 1;
    }
    free(data);
    return sink;
}
