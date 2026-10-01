// SPDX-License-Identifier: Apache-2.0
// visit(amount, index) adds amount to slots[index] and calls next(index + 1), which calls
// visit(1, index) back, up to the end of the array. start(first) calls visit(1, first); main calls
// start(0) and the thread start(2): both reach slots[2] and slots[3]. The index is visit()'s
// second parameter and next()'s first: around the cycle it is followed from the parameter it is
// computed from, not from its position, and the race is reported once (#159).
// Expected: one data race on `slots`, main's recursion against the thread's.
#include <pthread.h>
#include <stddef.h>

static int slots[4];

static void next(int index);

static void visit(int amount, int index)
{
    slots[index] += amount;
    if (index < 3)
        next(index + 1);
}

static void next(int index)
{
    visit(1, index);
}

static void start(int first)
{
    visit(1, first);
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
