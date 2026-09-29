// SPDX-License-Identifier: Apache-2.0
// record locks the ledger, which each of its callers already holds, so each thread waits for
// itself there. One caller also holds the audit lock around the call and the other does not: the
// calls differ, but the reacquisition is one, where record takes the ledger again (#136).
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t ledger = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t audit = PTHREAD_MUTEX_INITIALIZER;
static int total;

static void record(int amount)
{
    pthread_mutex_lock(&ledger);
    total += amount;
    pthread_mutex_unlock(&ledger);
}

static void deposit(int amount)
{
    pthread_mutex_lock(&ledger);
    record(amount);
    pthread_mutex_unlock(&ledger);
}

static void auditedDeposit(int amount)
{
    pthread_mutex_lock(&audit);
    pthread_mutex_lock(&ledger);
    record(amount);
    pthread_mutex_unlock(&ledger);
    pthread_mutex_unlock(&audit);
}

static void* worker(void* argument)
{
    (void)argument;
    deposit(1);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    auditedDeposit(2);
    pthread_join(thread, NULL);
    return total;
}
