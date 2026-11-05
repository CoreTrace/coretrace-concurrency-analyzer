// SPDX-License-Identifier: Apache-2.0
// The helper points the field at task after its read, and main calls it twice: the second thread
// runs on task (#242).
// Expected: at least main's read of task against writer. The race of the first thread on other
// is missed, and the threads racing with each other or with the call are not pinned.
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
    task->self = task;
}

int main(void)
{
    struct Task task = {0};
    struct Task other = {0};
    task.self = &other;
    for (int round = 0; round < 2; ++round)
        start(&task);
    int seen = task.value;
    int racing = other.value;
    pthread_join(task.thread, NULL);
    return seen + racing;
}
