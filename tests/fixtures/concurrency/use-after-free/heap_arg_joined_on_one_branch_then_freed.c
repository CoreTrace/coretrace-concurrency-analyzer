// SPDX-License-Identifier: Apache-2.0
// Only one branch of the `if` joins the thread before main frees `data`: on the other, the worker
// may still read it.
// Expected: one thread-arg-freed. The missing join and the race on `sink` are reported too.
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
    free(data);
    return sink;
}
