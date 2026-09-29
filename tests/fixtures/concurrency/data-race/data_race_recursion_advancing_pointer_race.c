// SPDX-License-Identifier: Apache-2.0
// A recursion walks an array by advancing its pointer and bumps every element without a lock, and
// a thread and main run it at the same time: the elements race. The recursion moves the pointer
// at every call; the analysis must terminate and still report the race (#117).
#include <pthread.h>
#include <stddef.h>

static int counters[4];

static void bumpFrom(int* counter, int remaining)
{
    *counter += 1;
    if (remaining > 1)
        bumpFrom(counter + 1, remaining - 1);
}

static void* worker(void* argument)
{
    (void)argument;
    bumpFrom(&counters[0], 4);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    bumpFrom(&counters[0], 4);
    pthread_join(thread, NULL);
    return counters[3];
}
