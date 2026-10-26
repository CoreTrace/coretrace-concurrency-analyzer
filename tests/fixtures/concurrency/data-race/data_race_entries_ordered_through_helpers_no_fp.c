// SPDX-License-Identifier: Apache-2.0
// Helpers run first, second, then first again, each joined before it returns: no two of them run
// at once (#224).
// Expected: no diagnostic.
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
    run_first();
    return shared;
}
