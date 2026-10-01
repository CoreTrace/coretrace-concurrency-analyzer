// SPDX-License-Identifier: Apache-2.0
// bump() increments slots[index + 1], and main calls bump(0): it increments slots[1], which the
// thread increments too. An index the helper computes from its parameter is not the parameter,
// so the element stays unknown, and the race stays reported (#159).
// Expected: one data race on `slots`, main's call to bump() against the thread's increment.
#include <pthread.h>
#include <stddef.h>

static int slots[3];

static void bump(int index)
{
    slots[index + 1] += 1;
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
    bump(0);
    pthread_join(thread, NULL);
    return slots[1];
}
