// SPDX-License-Identifier: Apache-2.0
// Two copies of denseWorker take d_0 to d_9 in both orders, always under c_gate: the gate
// serializes them, but those orders close more cycles than the search may walk from d_0 or d_1,
// so it stops early there. main and forwardWorker invert a_first and a_second, and e_first and
// e_second. main and pairWorker lock b_alice and b_bob lower address first, then b_ledger, main
// holding b_registry around it; they also lock f_first and f_second lower address first, which
// fixedWorker locks in a fixed order (#127).
// Expected: four deadlocks, the two inversions and the two pairs, and one notice that the
// lock-order cycle search is incomplete. The b_alice/b_bob pair alone cannot deadlock, but a
// longer cycle through it may be among those the search did not reach.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t a_first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t a_second = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t b_alice = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t b_bob = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t b_ledger = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t b_registry = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t c_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t d_0 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t d_1 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t d_2 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t d_3 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t d_4 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t d_5 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t d_6 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t d_7 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t d_8 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t d_9 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t e_first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t e_second = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t f_first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t f_second = PTHREAD_MUTEX_INITIALIZER;

static void lockPairByAddress(pthread_mutex_t* first, pthread_mutex_t* second)
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

static void lockAccountsByAddress(pthread_mutex_t* first, pthread_mutex_t* second)
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
    pthread_mutex_lock(&b_ledger);
    pthread_mutex_unlock(&b_ledger);
    pthread_mutex_unlock(second);
    pthread_mutex_unlock(first);
}

static void* denseWorker(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&c_gate);
    pthread_mutex_lock(&d_0);
    pthread_mutex_lock(&d_1);
    pthread_mutex_lock(&d_2);
    pthread_mutex_lock(&d_3);
    pthread_mutex_lock(&d_4);
    pthread_mutex_lock(&d_5);
    pthread_mutex_lock(&d_6);
    pthread_mutex_lock(&d_7);
    pthread_mutex_lock(&d_8);
    pthread_mutex_lock(&d_9);
    pthread_mutex_unlock(&d_9);
    pthread_mutex_unlock(&d_8);
    pthread_mutex_unlock(&d_7);
    pthread_mutex_unlock(&d_6);
    pthread_mutex_unlock(&d_5);
    pthread_mutex_unlock(&d_4);
    pthread_mutex_unlock(&d_3);
    pthread_mutex_unlock(&d_2);
    pthread_mutex_unlock(&d_1);
    pthread_mutex_unlock(&d_0);
    pthread_mutex_unlock(&c_gate);

    pthread_mutex_lock(&c_gate);
    pthread_mutex_lock(&d_9);
    pthread_mutex_lock(&d_8);
    pthread_mutex_lock(&d_7);
    pthread_mutex_lock(&d_6);
    pthread_mutex_lock(&d_5);
    pthread_mutex_lock(&d_4);
    pthread_mutex_lock(&d_3);
    pthread_mutex_lock(&d_2);
    pthread_mutex_lock(&d_1);
    pthread_mutex_lock(&d_0);
    pthread_mutex_unlock(&d_0);
    pthread_mutex_unlock(&d_1);
    pthread_mutex_unlock(&d_2);
    pthread_mutex_unlock(&d_3);
    pthread_mutex_unlock(&d_4);
    pthread_mutex_unlock(&d_5);
    pthread_mutex_unlock(&d_6);
    pthread_mutex_unlock(&d_7);
    pthread_mutex_unlock(&d_8);
    pthread_mutex_unlock(&d_9);
    pthread_mutex_unlock(&c_gate);
    return NULL;
}

static void* forwardWorker(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&a_first);
    pthread_mutex_lock(&a_second);
    pthread_mutex_unlock(&a_second);
    pthread_mutex_unlock(&a_first);

    pthread_mutex_lock(&e_first);
    pthread_mutex_lock(&e_second);
    pthread_mutex_unlock(&e_second);
    pthread_mutex_unlock(&e_first);
    return NULL;
}

static void* pairWorker(void* argument)
{
    (void)argument;
    lockAccountsByAddress(&b_bob, &b_alice);
    lockPairByAddress(&f_second, &f_first);
    return NULL;
}

static void* fixedWorker(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&f_first);
    pthread_mutex_lock(&f_second);
    pthread_mutex_unlock(&f_second);
    pthread_mutex_unlock(&f_first);
    return NULL;
}

int main(void)
{
    pthread_t dense;
    pthread_t otherDense;
    pthread_t forward;
    pthread_t pair;
    pthread_t fixed;
    pthread_create(&dense, NULL, denseWorker, NULL);
    pthread_create(&otherDense, NULL, denseWorker, NULL);
    pthread_create(&forward, NULL, forwardWorker, NULL);
    pthread_create(&pair, NULL, pairWorker, NULL);
    pthread_create(&fixed, NULL, fixedWorker, NULL);

    pthread_mutex_lock(&a_second);
    pthread_mutex_lock(&a_first);
    pthread_mutex_unlock(&a_first);
    pthread_mutex_unlock(&a_second);

    pthread_mutex_lock(&b_registry);
    lockAccountsByAddress(&b_alice, &b_bob);
    pthread_mutex_unlock(&b_registry);
    lockPairByAddress(&f_first, &f_second);

    pthread_mutex_lock(&e_second);
    pthread_mutex_lock(&e_first);
    pthread_mutex_unlock(&e_first);
    pthread_mutex_unlock(&e_second);

    pthread_join(dense, NULL);
    pthread_join(otherDense, NULL);
    pthread_join(forward, NULL);
    pthread_join(pair, NULL);
    pthread_join(fixed, NULL);
    return 0;
}


// EXPECT-HUMAN-DIAGNOSTICS-BEGIN
// Function: forwardWorker
// 	severity: ERROR
// 	ruleId: DeadlockLockOrder
// 	cwe: CWE-833
// 	at line 125, column 5
// 	[!!!Error] potential deadlock caused by inconsistent lock acquisition order
// 	     ↳ first order: acquire 'a_second' while holding 'a_first' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_search_reaches_exploration_limit.c:125:5 in forwardWorker (thread entries: forwardWorker)
// 	     ↳ conflicting order: acquire 'a_first' while holding 'a_second' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_search_reaches_exploration_limit.c:168:5 in main (thread entries: <main-task>)
// 	related: Conflicting lock order -> ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_search_reaches_exploration_limit.c:168:5 in main

// Function: forwardWorker
// 	severity: ERROR
// 	ruleId: DeadlockLockOrder
// 	cwe: CWE-833
// 	at line 130, column 5
// 	[!!!Error] potential deadlock caused by inconsistent lock acquisition order
// 	     ↳ first order: acquire 'e_second' while holding 'e_first' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_search_reaches_exploration_limit.c:130:5 in forwardWorker (thread entries: forwardWorker)
// 	     ↳ conflicting order: acquire 'e_first' while holding 'e_second' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_search_reaches_exploration_limit.c:178:5 in main (thread entries: <main-task>)
// 	related: Conflicting lock order -> ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_search_reaches_exploration_limit.c:178:5 in main

// Function: fixedWorker
// 	severity: ERROR
// 	ruleId: DeadlockLockOrder
// 	cwe: CWE-833
// 	at line 148, column 5
// 	[!!!Error] potential deadlock caused by inconsistent lock acquisition order
// 	     ↳ first order: acquire 'f_second' while holding 'f_first' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_search_reaches_exploration_limit.c:148:5 in fixedWorker (thread entries: fixedWorker)
// 	     ↳ conflicting order: acquire 'f_first' while holding 'f_second' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_search_reaches_exploration_limit.c:175:5 in main (thread entries: <main-task>)
// 	related: Conflicting lock order -> ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_search_reaches_exploration_limit.c:175:5 in main

// Function: main
// 	severity: ERROR
// 	ruleId: DeadlockLockOrder
// 	cwe: CWE-833
// 	at line 173, column 5
// 	[!!!Error] potential deadlock caused by inconsistent lock acquisition order
// 	     ↳ first order: acquire 'b_bob' while holding 'b_alice' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_search_reaches_exploration_limit.c:173:5 in main (thread entries: <main-task>)
// 	     ↳ conflicting order: acquire 'b_alice' while holding 'b_bob' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_search_reaches_exploration_limit.c:139:5 in pairWorker (thread entries: pairWorker)
// 	related: Conflicting lock order -> ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_search_reaches_exploration_limit.c:139:5 in pairWorker

// Notice: cycle-search-limit-reached
// 	ruleId: DeadlockLockOrder
// 	lock-order cycle search incomplete: exploration limit reached; some lock-order cycles may not be reported
// EXPECT-HUMAN-DIAGNOSTICS-END
