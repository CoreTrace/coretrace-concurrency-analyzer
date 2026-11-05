// SPDX-License-Identifier: Apache-2.0
// A thread started on task before the store may point the field back at task while the helper
// reads it (#242).
// Expected: two data races, on the field between main, repoint and start, and main's read of task
// against writer.
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

static void* repoint(void* argument)
{
    struct Task* task = argument;
    task->self = task;
    return NULL;
}

int main(void)
{
    struct Task task = {0};
    struct Task other = {0};
    pthread_t early;
    pthread_create(&early, NULL, repoint, &task);
    task.self = &other;
    start(&task);
    pthread_join(early, NULL);
    int seen = task.value;
    int racing = other.value;
    pthread_join(task.thread, NULL);
    return seen + racing;
}
