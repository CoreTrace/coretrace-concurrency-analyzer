// SPDX-License-Identifier: Apache-2.0
// add() takes an amount it does not use to index anything, and increments `total` through
// bump(&total). main and two threads call add() with constants, at the same time: they race on
// `total`. A call handing its callee only integers changes nothing about where the callee's own
// accesses are reported: the race is reported once, as for a call handing nothing (#159).
// Expected: one data race on `total`.
#include <pthread.h>
#include <stddef.h>

static int total;

static void bump(int* counter)
{
    *counter += 1;
}

static void add(int amount)
{
    (void)amount;
    bump(&total);
}

static void* first(void* argument)
{
    (void)argument;
    add(1);
    return NULL;
}

static void* second(void* argument)
{
    (void)argument;
    add(2);
    return NULL;
}

int main(void)
{
    pthread_t firstThread;
    pthread_t secondThread;
    pthread_create(&firstThread, NULL, first, NULL);
    pthread_create(&secondThread, NULL, second, NULL);
    add(3);
    pthread_join(firstThread, NULL);
    pthread_join(secondThread, NULL);
    return total;
}
