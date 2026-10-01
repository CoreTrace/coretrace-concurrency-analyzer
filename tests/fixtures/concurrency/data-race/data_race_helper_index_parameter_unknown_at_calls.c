// SPDX-License-Identifier: Apache-2.0
// bump() increments the element of `slots` its parameter picks, and neither call passes a value
// known before the program runs: main passes argc % 2, the thread what its argument points to.
// Both may name the same element, so the race is possible and stays reported (#159).
// Expected: one data race on `slots`, main's call to bump() against the thread's.
#include <pthread.h>
#include <stddef.h>

static int slots[2];

static void bump(int index)
{
    slots[index] += 1;
}

static void* worker(void* argument)
{
    bump(*(int*)argument);
    return NULL;
}

int main(int argc, char** argv)
{
    (void)argv;
    int which = argc % 2;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, &which);
    bump(argc % 2);
    pthread_join(thread, NULL);
    return slots[0] + slots[1];
}
