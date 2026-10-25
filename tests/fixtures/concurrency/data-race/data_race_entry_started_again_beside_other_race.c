// SPDX-License-Identifier: Apache-2.0
// first is joined before second starts, then started again while second runs: the two write
// shared at once. Two entries ordered in one place are not ordered for every instance (#224).
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

int main(void)
{
    pthread_t a;
    pthread_t b;
    pthread_t c;
    pthread_create(&a, NULL, first, NULL);
    pthread_join(a, NULL);
    pthread_create(&b, NULL, second, NULL);
    pthread_create(&c, NULL, first, NULL);
    pthread_join(b, NULL);
    pthread_join(c, NULL);
    return shared;
}
