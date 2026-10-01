// SPDX-License-Identifier: Apache-2.0
// Only the backward call orders the two locks by address; the forward call always takes first,
// then second. When second is at the lower address, the worker's backward call and main's forward
// call take the locks in opposite orders. Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;

static void work(int forward)
{
    if (!forward)
    {
        if (&second < &first)
        {
            pthread_mutex_lock(&second);
            pthread_mutex_lock(&first);
        }
        else
        {
            pthread_mutex_lock(&first);
            pthread_mutex_lock(&second);
        }
    }
    else
    {
        pthread_mutex_lock(&first);
        pthread_mutex_lock(&second);
    }
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);
}

static void* backward(void* argument)
{
    (void)argument;
    work(0);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, backward, NULL);
    work(1);
    pthread_join(thread, NULL);
    return 0;
}

// EXPECT-HUMAN-DIAGNOSTICS-BEGIN
// Function: work
// 	severity: ERROR
// 	ruleId: DeadlockLockOrder
// 	cwe: CWE-833
// 	at line 29, column 9
// 	[!!!Error] potential deadlock caused by inconsistent lock acquisition order
// 	     ↳ first order: acquire 'second' while holding 'first' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_address_order_on_one_path.c:29:9 in work (thread entries: backward)
// 	     ↳ conflicting order: acquire 'first' while holding 'second' at ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_address_order_on_one_path.c:18:13 in work (thread entries: backward)
// 	related: Conflicting lock order -> ${REPO_ROOT}/tests/fixtures/concurrency/deadlock/deadlock_address_order_on_one_path.c:18:13 in work
// EXPECT-HUMAN-DIAGNOSTICS-END
