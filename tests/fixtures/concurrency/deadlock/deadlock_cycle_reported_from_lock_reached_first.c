// SPDX-License-Identifier: Apache-2.0
// main holds a_outer while it takes zeta then alpha; the worker takes alpha then zeta. main holding
// zeta and the worker holding alpha can wait for each other. The search reaches zeta first, through
// a_outer, so the cycle is reported from main's order, as before every cycle was judged (#127).
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t a_outer = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t alpha = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t zeta = PTHREAD_MUTEX_INITIALIZER;

static void* worker(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&alpha);
    pthread_mutex_lock(&zeta);
    pthread_mutex_unlock(&zeta);
    pthread_mutex_unlock(&alpha);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    pthread_mutex_lock(&a_outer);
    pthread_mutex_lock(&zeta);
    pthread_mutex_lock(&alpha);
    pthread_mutex_unlock(&alpha);
    pthread_mutex_unlock(&zeta);
    pthread_mutex_unlock(&a_outer);
    pthread_join(thread, NULL);
    return 0;
}

// EXPECT-HUMAN-DIAGNOSTICS-BEGIN
// Function: main
// 	severity: ERROR
// 	ruleId: DeadlockLockOrder
// 	cwe: CWE-833
// 	at line 29, column 5
// 	[!!!Error] potential deadlock caused by inconsistent lock acquisition order
// 	     ↳ first order: acquire 'alpha' while holding 'zeta' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_reported_from_lock_reached_first.c:29:5 in main (thread entries: <main-task>)
// 	     ↳ conflicting order: acquire 'zeta' while holding 'alpha' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_reported_from_lock_reached_first.c:17:5 in worker (thread entries: worker)
// 	related: Conflicting lock order -> ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_cycle_reported_from_lock_reached_first.c:17:5 in worker
// EXPECT-HUMAN-DIAGNOSTICS-END
