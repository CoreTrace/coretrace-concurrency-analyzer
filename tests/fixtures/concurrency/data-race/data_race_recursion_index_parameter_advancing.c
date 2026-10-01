// SPDX-License-Identifier: Apache-2.0
// fill() increments slots[index], then calls itself on index + 1 up to the end of the array. The
// thread calls fill(2), which reaches slots[3], while main increments slots[3] itself. The index
// moves at every call, so around the recursion it is unknown: the analysis terminates, and the
// race stays reported (#159, as #117 did for a pointer).
// Expected: one data race on `slots`, main's increment against the thread's recursion.
#include <pthread.h>
#include <stddef.h>

static int slots[4];

static void fill(int index)
{
    slots[index] += 1;
    if (index < 3)
        fill(index + 1);
}

static void* worker(void* argument)
{
    (void)argument;
    fill(2);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    slots[3] += 1;
    pthread_join(thread, NULL);
    return slots[3];
}
