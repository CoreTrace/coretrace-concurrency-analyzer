// SPDX-License-Identifier: Apache-2.0
// The object reaches the helper through an outer one that writes nothing before handing it on:
// main's store is still what the helper reads (#242).
// Expected: one data race, main's read of other against writer.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    struct Task* self;
    pthread_t thread;
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

static void outer(struct Task* task)
{
    start(task);
}

int main(void)
{
    struct Task task = {0};
    struct Task other = {0};
    task.self = &other;
    outer(&task);
    int seen = task.value;
    int racing = other.value;
    pthread_join(task.thread, NULL);
    return seen + racing;
}
