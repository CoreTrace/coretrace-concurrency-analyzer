// SPDX-License-Identifier: Apache-2.0
// The ordinary shape of a race on heap state: the thread that hands the object over keeps using
// it. Both sides reach the object by reading the same variable, which is what lets the two
// accesses be compared at all — the allocation behind it has no name.
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
    pthread_t worker;

    pthread_create(&worker, NULL, increment, shared);
    shared->value = shared->value + 1;
    pthread_join(worker, NULL);

    free(shared);
    return 0;
}
