// SPDX-License-Identifier: Apache-2.0
// A helper main calls before the spawn points the field back at task (#242).
// Expected: one data race, main's read of task against writer.
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

static void point_at_itself(struct Task* task)
{
    task->self = task;
}

int main(void)
{
    struct Task task = {0};
    struct Task other = {0};
    task.self = &other;
    point_at_itself(&task);
    start(&task);
    int seen = task.value;
    int racing = other.value;
    pthread_join(task.thread, NULL);
    return seen + racing;
}
