// SPDX-License-Identifier: Apache-2.0
// touch_at() hands &slots[index] to publish(), which has no body, and main calls touch_at(1)
// while the thread increments slots[0]. A callee handed a pointer into an array may reach any of
// its elements, before the one it was handed as well, so knowing the index does not narrow what
// the call reaches (#159, as #99 for a pointer).
// Expected: one data race on `slots`, main's call to touch_at() against the thread's increment.
#include <pthread.h>
#include <stddef.h>

static int slots[2];

void publish(int* value);

static void touch_at(int index)
{
    publish(&slots[index]);
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
    touch_at(1);
    pthread_join(thread, NULL);
    return slots[0];
}
