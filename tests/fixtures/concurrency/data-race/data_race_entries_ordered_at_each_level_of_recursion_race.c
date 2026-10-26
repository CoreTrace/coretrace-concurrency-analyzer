// SPDX-License-Identifier: Apache-2.0
// Each level orders first before second, then recurses while its second runs: the next level's
// first and second run beside it. The recursion's instances are not those of one level (#224).
// Expected: two data races, first against second and second against itself.
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

static void phases(int depth)
{
    pthread_t a;
    pthread_t b;
    pthread_create(&a, NULL, first, NULL);
    pthread_join(a, NULL);
    pthread_create(&b, NULL, second, NULL);
    if (depth > 0)
        phases(depth - 1);
    pthread_join(b, NULL);
}

int main(void)
{
    phases(1);
    return shared;
}
