// SPDX-License-Identifier: Apache-2.0
// first is joined before second starts, and neither starts again: they never run at once (#224).
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

int main(void)
{
    pthread_t a;
    pthread_t b;
    pthread_create(&a, NULL, first, NULL);
    pthread_join(a, NULL);
    pthread_create(&b, NULL, second, NULL);
    pthread_join(b, NULL);
    return shared;
}
