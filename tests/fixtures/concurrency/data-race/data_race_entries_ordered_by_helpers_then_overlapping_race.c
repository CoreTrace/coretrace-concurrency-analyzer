// SPDX-License-Identifier: Apache-2.0
// Helpers run first, then second, each joined before it returns; main then starts both at once.
// The order the helpers give is not the order of every instance (#224).
// Expected: one data race, first against second.
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

static void run_first(void)
{
    pthread_t a;
    pthread_create(&a, NULL, first, NULL);
    pthread_join(a, NULL);
}

static void run_second(void)
{
    pthread_t b;
    pthread_create(&b, NULL, second, NULL);
    pthread_join(b, NULL);
}

int main(void)
{
    run_first();
    run_second();
    pthread_t a;
    pthread_t b;
    pthread_create(&a, NULL, first, NULL);
    pthread_create(&b, NULL, second, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    return shared;
}
