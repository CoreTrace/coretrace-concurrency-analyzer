// SPDX-License-Identifier: Apache-2.0
// main calls phases, which orders first before second, while a worker calls it through a
// pointer: called directly, phases is no root, and the worker's instances still run beside
// main's (#224).
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
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    phases();
    pthread_join(thread, NULL);
    return shared;
}
