// SPDX-License-Identifier: Apache-2.0
// bump() increments the element of `slots` its parameter picks. main calls bump(0), and the
// thread passes what its argument points to, which the analysis does not know: it may be 0, so
// the race is possible and stays reported (#159).
// Expected: one data race on `slots`, main's call to bump() against the thread's.
#include <pthread.h>
#include <stddef.h>

static int slots[2];
static int chosen;

static void bump(int index)
{
    slots[index] += 1;
}

static void* worker(void* argument)
{
    bump(*(int*)argument);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, &chosen);
    bump(0);
    pthread_join(thread, NULL);
    return slots[0];
}
