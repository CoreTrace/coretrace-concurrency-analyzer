// SPDX-License-Identifier: Apache-2.0
// The state lives on the heap, so it has no name a diagnostic could quote. What names it is the
// hand-off: both threads were given the same pointer, read from the same variable, so an access
// through one is an access to the same bytes as an access through the other.
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
    struct Counter* shared = malloc(sizeof *shared);
    pthread_t first;
    pthread_t second;

    pthread_create(&first, NULL, increment, shared);
    pthread_create(&second, NULL, increment, shared);
    pthread_join(first, NULL);
    pthread_join(second, NULL);

    free(shared);
    return 0;
}
