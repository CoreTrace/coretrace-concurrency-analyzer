// SPDX-License-Identifier: Apache-2.0
// main reaches bump(0) through reset_first(), and the thread calls bump(1): bump() increments
// the element of `slots` its parameter picks, so the two increment distinct elements (#159).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int slots[2];

static void bump(int index)
{
    slots[index] += 1;
}

static void reset_first(void)
{
    bump(0);
}

static void* worker(void* argument)
{
    (void)argument;
    bump(1);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    reset_first();
    pthread_join(thread, NULL);
    return slots[0] + slots[1];
}
