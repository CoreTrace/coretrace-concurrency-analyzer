// SPDX-License-Identifier: Apache-2.0
// both() hands each of its two parameters on to bump(), which increments the element of `slots`
// it picks. main calls both(0, 1) while the thread increments slots[1]: the second call races
// with the thread. Each index handed on is kept apart from the other (#159).
// Expected: one data race on `slots`, main's call to both() against the thread's increment.
#include <pthread.h>
#include <stddef.h>

static int slots[2];

static void bump(int index)
{
    slots[index] += 1;
}

static void both(int first, int second)
{
    bump(first);
    bump(second);
}

static void* worker(void* argument)
{
    (void)argument;
    slots[1] += 1;
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    both(0, 1);
    pthread_join(thread, NULL);
    return slots[1];
}
