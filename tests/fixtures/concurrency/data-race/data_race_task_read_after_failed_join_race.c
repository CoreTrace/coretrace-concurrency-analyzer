// SPDX-License-Identifier: Apache-2.0
// The owner reads the task's field on the branch where joining its thread failed. A failed join
// has waited for nothing, so on that branch the thread may still be running.
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

static void start(struct Task* task)
{
    pthread_create(&task->thread, NULL, run, task);
}

int main(void)
{
    struct Task task = {0};
    start(&task);
    if (pthread_join(task.thread, NULL) != 0)
        return task.value;
    return 0;
}
