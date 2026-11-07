// SPDX-License-Identifier: Apache-2.0
// main points the field at task, then at other, before the call: the last store is what the
// helper reads, and writer runs on other (#242).
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

int main(void)
{
    struct Task task = {0};
    struct Task other = {0};
    task.self = &task;
    task.self = &other;
    start(&task);
    int seen = task.value;
    int racing = other.value;
    pthread_join(task.thread, NULL);
    return seen + racing;
}
