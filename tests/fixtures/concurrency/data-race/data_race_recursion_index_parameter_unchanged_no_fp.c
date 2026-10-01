// SPDX-License-Identifier: Apache-2.0
// fill() increments slots[index], then calls itself on the same index until `rounds` runs out.
// main calls fill(0, 3) and the thread fill(1, 3): each recursion stays on its own element. The
// index goes around the recursion unchanged, so the analysis terminates and keeps it (#159).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

static int slots[2];

static void fill(int index, int rounds)
{
    slots[index] += 1;
    if (rounds > 1)
        fill(index, rounds - 1);
}

static void* worker(void* argument)
{
    (void)argument;
    fill(1, 3);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    fill(0, 3);
    pthread_join(thread, NULL);
    return slots[0] + slots[1];
}
