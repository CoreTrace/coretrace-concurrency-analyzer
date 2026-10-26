// SPDX-License-Identifier: Apache-2.0
// phases orders first before second, and two workers call it through a pointer at once: the
// first of one runs beside the second of the other. A function an indirect call enters may run
// in any thread (#224).
// Expected: a data race, first against second.
#include <pthread.h>
#include <stddef.h>

static int shared;

static void* first(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}

static void* second(void* argument)
{
    (void)argument;
    shared += 2;
    return NULL;
}

static void phases(void)
{
    pthread_t a;
    pthread_t b;
    pthread_create(&a, NULL, first, NULL);
    pthread_join(a, NULL);
    pthread_create(&b, NULL, second, NULL);
    pthread_join(b, NULL);
}

void (*volatile run_phases)(void) = phases;

static void* worker(void* argument)
{
    (void)argument;
    run_phases();
    return NULL;
}

int main(void)
{
    pthread_t one;
    pthread_t two;
    pthread_create(&one, NULL, worker, NULL);
    pthread_create(&two, NULL, worker, NULL);
    pthread_join(one, NULL);
    pthread_join(two, NULL);
    return shared;
}
