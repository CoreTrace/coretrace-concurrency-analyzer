// SPDX-License-Identifier: Apache-2.0
// bump() increments the element of `slots` its parameter picks, and main may call it with an
// index so large that its byte offset does not fit in 64 bits. That offset names no element the
// analysis can compute, so it stays unknown, and the thread's increment of slots[0] stays
// reported against it (#159).
// Expected: one data race on `slots`, main's call to bump() against the thread's increment.
#include <pthread.h>
#include <stddef.h>

static int slots[2];

static void bump(long index)
{
    slots[index] += 1;
}

static void* worker(void* argument)
{
    (void)argument;
    slots[0] += 1;
    return NULL;
}

int main(int argc, char** argv)
{
    (void)argv;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    if (argc > 1000)
        bump(0x3FFFFFFFFFFFFFFFL);
    pthread_join(thread, NULL);
    return slots[0];
}
