// SPDX-License-Identifier: Apache-2.0
// The memory is freed while the thread runs, but the thread never touches its argument, so
// nothing can be read after the free.
#include <pthread.h>
#include <stdlib.h>

static volatile int ticks = 0;

static void* worker(void* argument)
{
    (void)argument;
    ticks = 1;
    return NULL;
}

int main(void)
{
    int* value = malloc(sizeof *value);
    pthread_t thread;
    pthread_create(&thread, NULL, worker, value);
    free(value);
    pthread_join(thread, NULL);
    return ticks;
}
