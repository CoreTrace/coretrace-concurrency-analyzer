// SPDX-License-Identifier: Apache-2.0
// The object the helper is handed is a member of an outer one, whose other field also points at
// other: the field read is the member's own (#242).
// Expected: one data race, main's read of other against writer.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    struct Task* self;
    pthread_t thread;
};
struct Outer
{
    struct Task* decoy;
    struct Task inner;
};

static void* writer(void* argument)
{
    struct Task* task = argument;
    task->value += 1;
    return NULL;
}

static void start(struct Task* task)
{
    pthread_create(&task->thread, NULL, writer, task->self);
}

int main(void)
{
    struct Outer outer = {0};
    struct Task other = {0};
    outer.decoy = &other;
    outer.inner.self = &other;
    start(&outer.inner);
    int seen = outer.inner.value;
    int racing = other.value;
    pthread_join(outer.inner.thread, NULL);
    return seen + racing;
}
