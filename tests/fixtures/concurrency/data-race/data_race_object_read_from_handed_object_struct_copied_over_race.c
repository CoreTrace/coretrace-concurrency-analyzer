// SPDX-License-Identifier: Apache-2.0
// main copies over task a model whose field points at task, after pointing the field at other
// (#242).
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
    struct Task model = {0};
    model.self = &task;
    task.self = &other;
    task = model;
    start(&task);
    int seen = task.value;
    int racing = other.value;
    pthread_join(task.thread, NULL);
    return seen + racing;
}
