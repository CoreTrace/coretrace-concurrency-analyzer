// SPDX-License-Identifier: Apache-2.0
// update() calls bump(slots, which): bump() increments base[index], and update() hands its own
// parameter on as the index. main calls update(0) and the thread update(1): the array is named at
// the inner call and the element at the outer one, and the two increment distinct elements
// (#159).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int slots[2];

static void bump(int* base, int index)
{
    base[index] += 1;
}

static void update(int which)
{
    bump(slots, which);
}

static void* worker(void* argument)
{
    (void)argument;
    update(1);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    update(0);
    pthread_join(thread, NULL);
    return slots[0] + slots[1];
}
