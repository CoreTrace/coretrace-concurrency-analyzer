// SPDX-License-Identifier: Apache-2.0
// As data_race_entry_started_again_beside_other_race.c, the second start of first taking its
// entry from an array, which the order between the entries cannot resolve (#224).
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

static void* (*entries[])(void*) = {first};

int main(int argc, char** argv)
{
    (void)argv;
    pthread_t a;
    pthread_t b;
    pthread_t c;
    pthread_create(&a, NULL, first, NULL);
    pthread_join(a, NULL);
    pthread_create(&b, NULL, second, NULL);
    pthread_create(&c, NULL, entries[argc & 0], NULL);
    pthread_join(b, NULL);
    pthread_join(c, NULL);
    return shared;
}
