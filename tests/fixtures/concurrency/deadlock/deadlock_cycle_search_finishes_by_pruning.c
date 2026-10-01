// SPDX-License-Identifier: Apache-2.0
// clusterWorker, started once, takes c_0 to c_9 in both orders: their orders close many cycles,
// but all in that one thread, so no two of them may run in parallel. Two copies of
// hierarchyWorker take s_top, then h_00 to h_17, always in that order: many paths, no cycle.
// The search follows only orders that may run beside those it holds, toward locks that lead back
// to where it started, so it finishes. main and forwardWorker invert s_top and t_side (#127).
// Expected: one deadlock, the inversion, and no notice.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t c_0 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t c_1 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t c_2 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t c_3 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t c_4 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t c_5 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t c_6 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t c_7 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t c_8 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t c_9 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_00 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_01 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_02 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_03 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_04 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_05 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_06 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_07 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_08 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_09 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_10 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_11 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_12 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_13 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_14 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_15 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_16 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t h_17 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_top = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t t_side = PTHREAD_MUTEX_INITIALIZER;

static void* clusterWorker(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&c_0);
    pthread_mutex_lock(&c_1);
    pthread_mutex_lock(&c_2);
    pthread_mutex_lock(&c_3);
    pthread_mutex_lock(&c_4);
    pthread_mutex_lock(&c_5);
    pthread_mutex_lock(&c_6);
    pthread_mutex_lock(&c_7);
    pthread_mutex_lock(&c_8);
    pthread_mutex_lock(&c_9);
    pthread_mutex_unlock(&c_9);
    pthread_mutex_unlock(&c_8);
    pthread_mutex_unlock(&c_7);
    pthread_mutex_unlock(&c_6);
    pthread_mutex_unlock(&c_5);
    pthread_mutex_unlock(&c_4);
    pthread_mutex_unlock(&c_3);
    pthread_mutex_unlock(&c_2);
    pthread_mutex_unlock(&c_1);
    pthread_mutex_unlock(&c_0);

    pthread_mutex_lock(&c_9);
    pthread_mutex_lock(&c_8);
    pthread_mutex_lock(&c_7);
    pthread_mutex_lock(&c_6);
    pthread_mutex_lock(&c_5);
    pthread_mutex_lock(&c_4);
    pthread_mutex_lock(&c_3);
    pthread_mutex_lock(&c_2);
    pthread_mutex_lock(&c_1);
    pthread_mutex_lock(&c_0);
    pthread_mutex_unlock(&c_0);
    pthread_mutex_unlock(&c_1);
    pthread_mutex_unlock(&c_2);
    pthread_mutex_unlock(&c_3);
    pthread_mutex_unlock(&c_4);
    pthread_mutex_unlock(&c_5);
    pthread_mutex_unlock(&c_6);
    pthread_mutex_unlock(&c_7);
    pthread_mutex_unlock(&c_8);
    pthread_mutex_unlock(&c_9);
    return NULL;
}

static void* hierarchyWorker(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&s_top);
    pthread_mutex_lock(&h_00);
    pthread_mutex_lock(&h_01);
    pthread_mutex_lock(&h_02);
    pthread_mutex_lock(&h_03);
    pthread_mutex_lock(&h_04);
    pthread_mutex_lock(&h_05);
    pthread_mutex_lock(&h_06);
    pthread_mutex_lock(&h_07);
    pthread_mutex_lock(&h_08);
    pthread_mutex_lock(&h_09);
    pthread_mutex_lock(&h_10);
    pthread_mutex_lock(&h_11);
    pthread_mutex_lock(&h_12);
    pthread_mutex_lock(&h_13);
    pthread_mutex_lock(&h_14);
    pthread_mutex_lock(&h_15);
    pthread_mutex_lock(&h_16);
    pthread_mutex_lock(&h_17);
    pthread_mutex_unlock(&h_17);
    pthread_mutex_unlock(&h_16);
    pthread_mutex_unlock(&h_15);
    pthread_mutex_unlock(&h_14);
    pthread_mutex_unlock(&h_13);
    pthread_mutex_unlock(&h_12);
    pthread_mutex_unlock(&h_11);
    pthread_mutex_unlock(&h_10);
    pthread_mutex_unlock(&h_09);
    pthread_mutex_unlock(&h_08);
    pthread_mutex_unlock(&h_07);
    pthread_mutex_unlock(&h_06);
    pthread_mutex_unlock(&h_05);
    pthread_mutex_unlock(&h_04);
    pthread_mutex_unlock(&h_03);
    pthread_mutex_unlock(&h_02);
    pthread_mutex_unlock(&h_01);
    pthread_mutex_unlock(&h_00);
    pthread_mutex_unlock(&s_top);
    return NULL;
}

static void* forwardWorker(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&s_top);
    pthread_mutex_lock(&t_side);
    pthread_mutex_unlock(&t_side);
    pthread_mutex_unlock(&s_top);
    return NULL;
}

int main(void)
{
    pthread_t cluster;
    pthread_t hierarchy;
    pthread_t otherHierarchy;
    pthread_t forward;
    pthread_create(&cluster, NULL, clusterWorker, NULL);
    pthread_create(&hierarchy, NULL, hierarchyWorker, NULL);
    pthread_create(&otherHierarchy, NULL, hierarchyWorker, NULL);
    pthread_create(&forward, NULL, forwardWorker, NULL);
    pthread_mutex_lock(&t_side);
    pthread_mutex_lock(&s_top);
    pthread_mutex_unlock(&s_top);
    pthread_mutex_unlock(&t_side);
    pthread_join(cluster, NULL);
    pthread_join(hierarchy, NULL);
    pthread_join(otherHierarchy, NULL);
    pthread_join(forward, NULL);
    return 0;
}

// EXPECT-HUMAN-DIAGNOSTICS-BEGIN
// Function: forwardWorker
// 	severity: ERROR
// 	ruleId: DeadlockLockOrder
// 	cwe: CWE-833
// 	at line 137, column 5
// 	[!!!Error] potential deadlock caused by inconsistent lock acquisition order
// 	     ↳ first order: acquire 't_side' while holding 's_top' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_search_finishes_by_pruning.c:137:5 in forwardWorker (thread entries: forwardWorker)
// 	     ↳ conflicting order: acquire 's_top' while holding 't_side' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_search_finishes_by_pruning.c:154:5 in main (thread entries: <main-task>)
// 	related: Conflicting lock order -> ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_search_finishes_by_pruning.c:154:5 in main
// EXPECT-HUMAN-DIAGNOSTICS-END
