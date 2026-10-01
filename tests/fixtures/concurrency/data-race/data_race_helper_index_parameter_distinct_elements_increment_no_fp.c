// SPDX-License-Identifier: Apache-2.0
// bump() increments the element of `slots` its parameter picks. main calls bump(0) and the
// thread bump(1), at the same time: they increment distinct elements (#159).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int slots[2];

static void bump(int index)
{
    slots[index] += 1;
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
    bump(0);
    pthread_join(thread, NULL);
    return slots[0] + slots[1];
}
