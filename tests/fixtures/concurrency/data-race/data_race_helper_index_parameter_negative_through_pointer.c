// SPDX-License-Identifier: Apache-2.0
// bump() increments the element before slots[1] that its parameter picks, through a pointer to
// slots[1]. main calls bump(-1), which increments slots[0], and the thread increments slots[0]
// itself. A negative index is valid there; it is not read as a large unsigned one (#159).
// Expected: one data race on `slots`, main's call to bump() against the thread's increment.
#include <pthread.h>
#include <stddef.h>

static int slots[2];

static void bump(int index)
{
    int* second = &slots[1];
    second[index] += 1;
}

static void* worker(void* argument)
{
    (void)argument;
    slots[0] += 1;
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    bump(-1);
    pthread_join(thread, NULL);
    return slots[0];
}
