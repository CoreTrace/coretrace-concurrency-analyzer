// SPDX-License-Identifier: Apache-2.0
// main joins first before second starts, and second starts its own first, which writes shared
// beside it: an instance of first that main's order does not reach (#224).
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
    pthread_t child;
    pthread_create(&child, NULL, first, NULL);
    shared += 2;
    pthread_join(child, NULL);
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
