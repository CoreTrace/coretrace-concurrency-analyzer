// SPDX-License-Identifier: Apache-2.0
// start() hands its parameter to fill(), which increments slots[index], then calls itself on
// index + 1. main calls start(0) and the thread start(2): both recursions reach slots[2] and
// slots[3]. The recursion's accesses follow the index through start() to each call of it, and
// the race is reported once (#159).
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

static void start(int first)
{
    fill(first);
}

static void* worker(void* argument)
{
    (void)argument;
    start(2);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    start(0);
    pthread_join(thread, NULL);
    return slots[3];
}
