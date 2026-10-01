// SPDX-License-Identifier: Apache-2.0
// fill() increments slots[index], then calls itself on index + 1 up to the end of the array. main
// calls fill(0) and the thread fill(2): both recursions reach slots[2] and slots[3]. The index
// moves around the recursion, so there it is unknown, and each recursion's accesses stay those
// of the call that started it: the race is reported once (#159).
// Expected: one data race on `slots`, main's recursion against the thread's.
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
    fill(0);
    pthread_join(thread, NULL);
    return slots[3];
}
