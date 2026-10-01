// SPDX-License-Identifier: Apache-2.0
// fill() increments slots[index], then calls itself on index - 1 down to the first element. main
// calls fill(3) and the thread fill(1): both recursions reach slots[1] and slots[0]. The race is
// reported once (#159).
// Expected: one data race on `slots`, main's recursion against the thread's.
#include <pthread.h>
#include <stddef.h>

static int slots[4];

static void fill(int index)
{
    slots[index] += 1;
    if (index > 0)
        fill(index - 1);
}

static void* worker(void* argument)
{
    (void)argument;
    fill(1);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    fill(3);
    pthread_join(thread, NULL);
    return slots[0];
}
