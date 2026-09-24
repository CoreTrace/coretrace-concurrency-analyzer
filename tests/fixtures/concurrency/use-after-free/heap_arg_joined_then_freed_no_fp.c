// SPDX-License-Identifier: Apache-2.0
// The creator waits for the thread before freeing what it handed over, so the thread is done
// with the memory by the time it goes.
#include <pthread.h>
#include <stdlib.h>

static void* worker(void* argument)
{
    int* value = argument;
    return (void*)(long)*value;
}

int main(void)
{
    int* value = malloc(sizeof *value);
    *value = 1;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, value);
    pthread_join(thread, NULL);
    free(value);
    return 0;
}
