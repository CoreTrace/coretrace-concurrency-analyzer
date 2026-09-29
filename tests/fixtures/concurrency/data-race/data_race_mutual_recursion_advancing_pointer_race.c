// SPDX-License-Identifier: Apache-2.0
// Two functions call each other, one of them advancing the pointer, and bump the elements they
// reach without a lock while a thread and main both run them: the elements race. The pointer
// moves around a cycle of two functions rather than in a single recursive call; the analysis must
// terminate and still report the race (#117).
#include <pthread.h>
#include <stddef.h>

static int counters[4];

static void bumpEven(int* counter, int remaining);

static void bumpOdd(int* counter, int remaining)
{
    *counter += 1;
    if (remaining > 1)
        bumpEven(counter + 1, remaining - 1);
}

static void bumpEven(int* counter, int remaining)
{
    *counter += 2;
    if (remaining > 1)
        bumpOdd(counter, remaining - 1);
}

static void* worker(void* argument)
{
    (void)argument;
    bumpOdd(&counters[0], 4);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    bumpOdd(&counters[0], 4);
    pthread_join(thread, NULL);
    return counters[3];
}
