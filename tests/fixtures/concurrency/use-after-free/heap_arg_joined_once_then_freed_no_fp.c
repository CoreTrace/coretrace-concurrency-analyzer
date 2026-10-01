// SPDX-License-Identifier: Apache-2.0
// The thread is joined once, before main frees `data`.
// Expected: no diagnostic.
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
    (void)argc;
    (void)argv;
    int* data = malloc(sizeof *data);
    *data = 1;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, data);
    pthread_join(thread, NULL);
    free(data);
    return sink;
}
