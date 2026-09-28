// SPDX-License-Identifier: Apache-2.0
// The owner hands a heap object to a thread through a local pointer, then points that variable
// at another object. The write is to the variable, not to the object the thread works on.
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

struct Shared
{
    int value;
};

static void* worker(void* argument)
{
    struct Shared* data = argument;
    data->value += 1;
    return NULL;
}

int main(void)
{
    struct Shared* shared = calloc(1, sizeof *shared);
    pthread_t thread;
    pthread_create(&thread, NULL, worker, shared);
    struct Shared* first = shared;
    shared = calloc(1, sizeof *shared);
    pthread_join(thread, NULL);
    free(first);
    free(shared);
    return 0;
}
