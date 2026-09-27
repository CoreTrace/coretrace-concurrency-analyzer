// SPDX-License-Identifier: Apache-2.0
// The owner writes shared.in.y through a local pointer to shared.in. The pointer is kept in a
// local variable, and the offset of `in` must survive that copy: the write lands where the
// worker's does.
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
    inner->y = 5;
    pthread_join(thread, NULL);
    return shared.in.y;
}
