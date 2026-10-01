// SPDX-License-Identifier: Apache-2.0
// bump() increments base[index]. main calls bump(slots, 1) and the thread bump(&slots[1], 0):
// both increment slots[1] (#159).
// Expected: one data race on `slots`, main's call to bump() against the thread's.
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
    bump(&slots[1], 0);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    bump(slots, 1);
    pthread_join(thread, NULL);
    return slots[1];
}
