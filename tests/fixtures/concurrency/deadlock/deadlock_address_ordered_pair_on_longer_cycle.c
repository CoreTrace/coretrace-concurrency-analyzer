// SPDX-License-Identifier: Apache-2.0
// main takes a_west then b_zone, and a westward worker takes them the other way: one cycle. Two
// workers take c_first and d_second lower address first, and a zone worker takes d_second then
// b_zone. The pair alone cannot deadlock: both its orders are taken by address, so every worker
// takes the lower lock first. When c_first is the lower, main holding a_west (it takes c_first
// next), a pair worker holding c_first, the zone worker holding d_second and the westward worker
// holding b_zone can wait for each other in a ring through the pair.
// Expected: two deadlocks, the a_west/b_zone inversion and the four-lock ring, none on the pair.
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

// EXPECT-HUMAN-DIAGNOSTICS-BEGIN
// Function: main
// 	severity: ERROR
// 	ruleId: DeadlockLockOrder
// 	cwe: CWE-833
// 	at line 78, column 5
// 	[!!!Error] potential deadlock caused by inconsistent lock acquisition order
// 	     ↳ first order: acquire 'b_zone' while holding 'a_west' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_address_ordered_pair_on_longer_cycle.c:78:5 in main (thread entries: <main-task>)
// 	     ↳ conflicting order: acquire 'a_west' while holding 'b_zone' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_address_ordered_pair_on_longer_cycle.c:61:5 in westWorker (thread entries: westWorker)
// 	related: Conflicting lock order -> ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_address_ordered_pair_on_longer_cycle.c:61:5 in westWorker

// Function: main
// 	severity: ERROR
// 	ruleId: DeadlockLockOrder
// 	cwe: CWE-833
// 	at line 80, column 5
// 	[!!!Error] potential deadlock caused by a cycle of 4 lock acquisitions
// 	     ↳ first order: acquire 'c_first' while holding 'a_west' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_address_ordered_pair_on_longer_cycle.c:80:5 in main (thread entries: <main-task>)
// 	     ↳ conflicting order: acquire 'd_second' while holding 'c_first' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_address_ordered_pair_on_longer_cycle.c:22:9 in lockPairByAddress (thread entries: otherPairWorker, pairWorker)
// 	     ↳ conflicting order: acquire 'b_zone' while holding 'd_second' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_address_ordered_pair_on_longer_cycle.c:51:5 in zoneWorker (thread entries: zoneWorker)
// 	     ↳ conflicting order: acquire 'a_west' while holding 'b_zone' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_address_ordered_pair_on_longer_cycle.c:61:5 in westWorker (thread entries: westWorker)
// 	related: Conflicting lock order -> ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_address_ordered_pair_on_longer_cycle.c:22:9 in lockPairByAddress
// 	related: Conflicting lock order -> ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_address_ordered_pair_on_longer_cycle.c:51:5 in zoneWorker
// 	related: Conflicting lock order -> ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_address_ordered_pair_on_longer_cycle.c:61:5 in westWorker
// EXPECT-HUMAN-DIAGNOSTICS-END
