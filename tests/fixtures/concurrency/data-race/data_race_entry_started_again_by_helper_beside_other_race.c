// SPDX-License-Identifier: Apache-2.0
// As data_race_entry_started_again_beside_other_race.c, first started by a helper each time (#224).
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

static void start_first(pthread_t* handle)
{
    pthread_create(handle, NULL, first, NULL);
}

int main(void)
{
    pthread_t a;
    pthread_t b;
    pthread_t c;
    start_first(&a);
    pthread_join(a, NULL);
    pthread_create(&b, NULL, second, NULL);
    start_first(&c);
    pthread_join(b, NULL);
    pthread_join(c, NULL);
    return shared;
}
