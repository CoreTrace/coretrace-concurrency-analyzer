// SPDX-License-Identifier: Apache-2.0
// update() calls bump(&slots[start], which): bump() increments base[index]. main calls
// update(0, argc % 2) while the thread increments slots[1]. The index is known, but the element
// it counts from is not, so the element reached stays unknown, and the race stays reported
// (#159).
// Expected: one data race on `slots`, main's call to update() against the thread's increment.
#include <pthread.h>
#include <stddef.h>

static int slots[3];

static void bump(int* base, int index)
{
    base[index] += 1;
}

static void update(int which, int start)
{
    bump(&slots[start], which);
}

static void* worker(void* argument)
{
    (void)argument;
    slots[1] += 1;
    return NULL;
}

int main(int argc, char** argv)
{
    (void)argv;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    update(0, argc % 2);
    pthread_join(thread, NULL);
    return slots[1];
}
