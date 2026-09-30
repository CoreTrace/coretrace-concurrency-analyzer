// SPDX-License-Identifier: Apache-2.0
// Three workers each lock two of three mutexes through one helper, lower address first. Addresses
// order the three mutexes totally, so every worker takes each pair the same way and no ring of
// workers can close (#138).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t north = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t east = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t south = PTHREAD_MUTEX_INITIALIZER;

void lockPairByAddress(pthread_mutex_t* first, pthread_mutex_t* second)
{
    if (first < second)
    {
        pthread_mutex_lock(first);
        pthread_mutex_lock(second);
    }
    else
    {
        pthread_mutex_lock(second);
        pthread_mutex_lock(first);
    }
    pthread_mutex_unlock(second);
    pthread_mutex_unlock(first);
}

void* northEast(void* argument)
{
    (void)argument;
    lockPairByAddress(&north, &east);
    return NULL;
}

void* eastSouth(void* argument)
{
    (void)argument;
    lockPairByAddress(&east, &south);
    return NULL;
}

void* southNorth(void* argument)
{
    (void)argument;
    lockPairByAddress(&south, &north);
    return NULL;
}

int main(void)
{
    pthread_t first;
    pthread_t second;
    pthread_t third;
    pthread_create(&first, NULL, northEast, NULL);
    pthread_create(&second, NULL, eastSouth, NULL);
    pthread_create(&third, NULL, southNorth, NULL);
    pthread_join(first, NULL);
    pthread_join(second, NULL);
    pthread_join(third, NULL);
    return 0;
}
