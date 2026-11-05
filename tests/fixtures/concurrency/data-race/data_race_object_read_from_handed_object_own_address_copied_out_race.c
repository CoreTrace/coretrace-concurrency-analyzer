// SPDX-License-Identifier: Apache-2.0
// main copies task, field included, then points the field back at task through the copy (#242).
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

int main(void)
{
    struct Task task = {0};
    struct Task other = {0};
    task.self = &task;
    struct Task copy = task;
    task.self = &other;
    copy.self->self = copy.self;
    start(&task);
    int seen = task.value;
    int racing = other.value;
    pthread_join(task.thread, NULL);
    return seen + racing;
}
