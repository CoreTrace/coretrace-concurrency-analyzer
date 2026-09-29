// SPDX-License-Identifier: Apache-2.0
// main takes a_west then b_zone, and a westward worker takes them the other way: one cycle. Two
// workers take c_first and d_second lower address first, and a zone worker takes d_second then
// b_zone. When c_first is the lower, main holding a_west, a pair worker holding c_first, the zone
// worker holding d_second and the westward worker holding b_zone can wait for each other in a
// ring. The search reaches b_zone before that ring and never finds it again: the report on
// c_first and d_second, which the ring runs through, is the one that stands for it.
// Expected: two deadlocks, the a_west/b_zone inversion and the c_first/d_second pair.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t a_west = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t b_zone = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t c_first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t d_second = PTHREAD_MUTEX_INITIALIZER;

static void lockPairByAddress(void)
{
    if (&c_first < &d_second)
    {
        pthread_mutex_lock(&c_first);
        pthread_mutex_lock(&d_second);
    }
    else
    {
        pthread_mutex_lock(&d_second);
        pthread_mutex_lock(&c_first);
    }
    pthread_mutex_unlock(&d_second);
    pthread_mutex_unlock(&c_first);
}

static void* pairWorker(void* argument)
{
    (void)argument;
    lockPairByAddress();
    return NULL;
}

static void* otherPairWorker(void* argument)
{
    (void)argument;
    lockPairByAddress();
    return NULL;
}

static void* zoneWorker(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&d_second);
    pthread_mutex_lock(&b_zone);
    pthread_mutex_unlock(&b_zone);
    pthread_mutex_unlock(&d_second);
    return NULL;
}

static void* westWorker(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&b_zone);
    pthread_mutex_lock(&a_west);
    pthread_mutex_unlock(&a_west);
    pthread_mutex_unlock(&b_zone);
    return NULL;
}

int main(void)
{
    pthread_t pair;
    pthread_t otherPair;
    pthread_t zone;
    pthread_t west;
    pthread_create(&pair, NULL, pairWorker, NULL);
    pthread_create(&otherPair, NULL, otherPairWorker, NULL);
    pthread_create(&zone, NULL, zoneWorker, NULL);
    pthread_create(&west, NULL, westWorker, NULL);
    pthread_mutex_lock(&a_west);
    pthread_mutex_lock(&b_zone);
    pthread_mutex_unlock(&b_zone);
    pthread_mutex_lock(&c_first);
    pthread_mutex_unlock(&c_first);
    pthread_mutex_unlock(&a_west);
    pthread_join(pair, NULL);
    pthread_join(otherPair, NULL);
    pthread_join(zone, NULL);
    pthread_join(west, NULL);
    return 0;
}
