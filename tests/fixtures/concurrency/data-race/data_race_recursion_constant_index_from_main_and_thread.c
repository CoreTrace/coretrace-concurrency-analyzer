// SPDX-License-Identifier: Apache-2.0
// fill() increments slots[index], then calls fill(0) once when index is not 0. main calls fill(0)
// and the thread fill(3), which reaches slots[0] through its recursive call: they race on
// slots[0]. A recursive call passing a constant is widened like any other, and the race is
// reported once (#159).
// Expected: one data race on `slots`, main's call against the thread's recursion.
#include <pthread.h>
#include <stddef.h>

static int slots[4];

static void fill(int index)
{
    slots[index] += 1;
    if (index != 0)
        fill(0);
}

static void* worker(void* argument)
{
    (void)argument;
    fill(3);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    fill(0);
    pthread_join(thread, NULL);
    return slots[0];
}
