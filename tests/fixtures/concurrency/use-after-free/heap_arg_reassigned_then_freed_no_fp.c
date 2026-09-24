// SPDX-License-Identifier: Apache-2.0
// The variable is reassigned after the thread starts, so what the creator frees before the join
// is another allocation than the one the thread was given.
#include <pthread.h>
#include <stdlib.h>

static void* worker(void* argument)
{
    int* value = argument;
    return (void*)(long)*value;
}

int main(void)
{
    int* given = malloc(sizeof *given);
    *given = 1;
    int* value = given;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, value);
    value = malloc(sizeof *value);
    free(value);
    pthread_join(thread, NULL);
    free(given);
    return 0;
}
