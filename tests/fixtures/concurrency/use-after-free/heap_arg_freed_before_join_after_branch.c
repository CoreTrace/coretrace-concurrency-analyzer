// SPDX-License-Identifier: Apache-2.0
// main frees `data` just before joining the thread, in the block where a branch rejoins: the worker
// may still read it there.
// Expected: one thread-arg-freed.
#include <pthread.h>
#include <stdio.h>
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
        puts("started");
    free(data);
    pthread_join(thread, NULL);
    return sink;
}
