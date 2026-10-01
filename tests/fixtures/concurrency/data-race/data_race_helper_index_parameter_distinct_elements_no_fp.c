// SPDX-License-Identifier: Apache-2.0
// put() writes the element of `slots` its parameter picks. main calls put(0) and the thread
// put(1), at the same time: they write distinct elements (#159).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int slots[2];

static void put(int index)
{
    slots[index] = 1;
}

static void* worker(void* argument)
{
    (void)argument;
    put(1);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    put(0);
    pthread_join(thread, NULL);
    return slots[0] + slots[1];
}
