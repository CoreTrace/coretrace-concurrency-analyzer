// SPDX-License-Identifier: Apache-2.0
// Two local copies, each taken after an indexing step: the offset of `in` and then of `y`
// must both survive, so the write through the second pointer still lands on the worker's field.
#include <pthread.h>
#include <stddef.h>

struct Inner
{
    int x;
    int y;
};

struct Outer
{
    int a;
    struct Inner in;
};

static struct Outer shared;

static void* worker(void* argument)
{
    (void)argument;
    shared.in.y += 1;
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    struct Inner* inner = &shared.in;
    int* slot = &inner->y;
    *slot = 5;
    pthread_join(thread, NULL);
    return shared.in.y;
}
