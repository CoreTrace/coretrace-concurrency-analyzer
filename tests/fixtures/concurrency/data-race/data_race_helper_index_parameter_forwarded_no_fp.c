// SPDX-License-Identifier: Apache-2.0
// update() hands its own parameter on to bump(), which increments the element of `slots` it
// picks. main calls update(0) and the thread update(1): distinct elements (#159).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int slots[2];

static void bump(int index)
{
    slots[index] += 1;
}

static void update(int which)
{
    bump(which);
}

static void* worker(void* argument)
{
    (void)argument;
    update(1);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    update(0);
    pthread_join(thread, NULL);
    return slots[0] + slots[1];
}
