// SPDX-License-Identifier: Apache-2.0
// The control for data_race_recursion_index_advancing_from_main_and_thread.c, with a pointer:
// fill() increments *slot, then calls itself on slot + 1 up to the end of the array. main calls
// fill(&slots[0]) and the thread fill(&slots[2]): both recursions reach slots[2] and slots[3],
// and the race is reported once (#159, #118).
// Expected: one data race on `slots`, main's recursion against the thread's.
#include <pthread.h>
#include <stddef.h>

static int slots[4];

static void fill(int* slot)
{
    *slot += 1;
    if (slot < &slots[3])
        fill(slot + 1);
}

static void* worker(void* argument)
{
    (void)argument;
    fill(&slots[2]);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    fill(&slots[0]);
    pthread_join(thread, NULL);
    return slots[3];
}
