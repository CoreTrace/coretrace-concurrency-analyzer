// SPDX-License-Identifier: Apache-2.0
// Two workers lock north and east, then east and south, through a helper that takes the lower
// address first; the third locks south then north in that fixed order. One order of the ring is
// not taken by address, so a layout closes it: when north sits below east and east below south,
// the three workers, holding north, east and south respectively, can wait for each other (#138).
// Expected: one deadlock, the three-lock ring.
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
    pthread_mutex_lock(&south);
    pthread_mutex_lock(&north);
    pthread_mutex_unlock(&north);
    pthread_mutex_unlock(&south);
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
