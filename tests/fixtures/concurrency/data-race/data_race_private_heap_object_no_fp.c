// SPDX-License-Identifier: Apache-2.0
// The correct counterpart: each thread is handed its own allocation, read from its own variable.
// Nothing is shared, and the identical body must stay silent.
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

struct Counter
{
    int value;
};

static void* increment(void* argument)
{
    struct Counter* counter = (struct Counter*)argument;
    counter->value = counter->value + 1;
    return NULL;
}

int main(void)
{
    struct Counter* first_counter = malloc(sizeof *first_counter);
    struct Counter* second_counter = malloc(sizeof *second_counter);
    pthread_t first;
    pthread_t second;

    pthread_create(&first, NULL, increment, first_counter);
    pthread_create(&second, NULL, increment, second_counter);
    pthread_join(first, NULL);
    pthread_join(second, NULL);

    free(first_counter);
    free(second_counter);
    return 0;
}
