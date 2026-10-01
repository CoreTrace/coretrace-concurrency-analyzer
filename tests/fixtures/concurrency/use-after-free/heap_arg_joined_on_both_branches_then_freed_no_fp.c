// SPDX-License-Identifier: Apache-2.0
// Each branch of the `if` joins the thread before main frees `data`, so the worker has finished
// with it, although neither join dominates the free.
// Expected: no thread-arg-freed. The missing join (#143) and the race on `sink` (#113) are
// reported elsewhere.
#include <pthread.h>
#include <stdlib.h>
static int sink;
static void* worker(void* argument)
{
    sink = *(int*)argument;
    return NULL;
}
int main(int argc, char** argv)
{
    (void)argv;
    int* data = malloc(sizeof *data);
    *data = 1;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, data);
    if (argc > 1)
        pthread_join(thread, NULL);
    else
        pthread_join(thread, NULL);
    free(data);
    return sink;
}
