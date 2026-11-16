// SPDX-License-Identifier: Apache-2.0
// A callee writes what the thread main started writes, before joining it and starting another: the
// write runs beside the first thread (#201).
// Expected: restart races with run. The race of run with itself, false (#252), is not pinned.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
};

static void* run(void* argument)
{
    struct Task* task = argument;
    task->value += 1;
    return NULL;
}

static void restart(struct Task* task)
{
    task->value += 1;
    pthread_join(task->thread, NULL);
    pthread_create(&task->thread, NULL, run, task);
}

int main(void)
{
    struct Task task = {0};
    pthread_create(&task.thread, NULL, run, &task);
    restart(&task);
    pthread_join(task.thread, NULL);
    return task.value;
}
