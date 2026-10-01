// SPDX-License-Identifier: Apache-2.0
// visit() increments slots[index] and calls next(index + 1), which calls visit() back, up to the
// end of the array. main calls visit(0) and the thread visit(2): both reach slots[2] and slots[3].
// The index moves around the cycle the two functions form, and the race is reported once (#159).
// Expected: one data race on `slots`, main's recursion against the thread's.
#include <pthread.h>
#include <stddef.h>

static int slots[4];

static void next(int index);

static void visit(int index)
{
    slots[index] += 1;
    if (index < 3)
        next(index + 1);
}

static void next(int index)
{
    visit(index);
}

static void* worker(void* argument)
{
    (void)argument;
    visit(2);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    visit(0);
    pthread_join(thread, NULL);
    return slots[3];
}
