// SPDX-License-Identifier: Apache-2.0
// Threading unit: every thread it starts is waited for by `join_worker`, defined in another unit.
// On its own it cannot know that, so a local handed to a thread escapes, freed memory is still in
// use, and the result races; the program shows all three threads finished at the call.
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

void join_worker(pthread_t thread);

static int result;
static _Thread_local int slot;
static int* published;

static void* bump_local(void* argument)
{
    int* value = (int*)argument;
    *value += 1;
    return NULL;
}

static void* bump_heap(void* argument)
{
    int* value = (int*)argument;
    *value += 1;
    return NULL;
}

static void* produce(void* argument)
{
    (void)argument;
    result = 42;
    slot = 1;
    published = &slot;
    return NULL;
}

static void run_on_stack(void)
{
    int local_value = 0;
    pthread_t thread;
    pthread_create(&thread, NULL, bump_local, &local_value);
    join_worker(thread);
}

static void run_on_heap(void)
{
    int* value = malloc(sizeof *value);
    if (value == NULL)
        return;
    *value = 0;

    pthread_t thread;
    pthread_create(&thread, NULL, bump_heap, value);
    join_worker(thread);
    free(value);
}

int main(void)
{
    run_on_stack();
    run_on_heap();

    pthread_t thread;
    pthread_create(&thread, NULL, produce, NULL);
    join_worker(thread);
    // The thread that owned `slot` has ended: reading through `published` is the one real defect.
    return result + *published;
}
