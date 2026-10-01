// SPDX-License-Identifier: Apache-2.0
// bump() increments base[index]. main calls bump(slots, 0) and the thread bump(slots, 1): the
// pointer names the array and the index the element, so the two increment distinct elements
// (#159).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int slots[2];

static void bump(int* base, int index)
{
    base[index] += 1;
}

static void* worker(void* argument)
{
    (void)argument;
    bump(slots, 1);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    bump(slots, 0);
    pthread_join(thread, NULL);
    return slots[0] + slots[1];
}
