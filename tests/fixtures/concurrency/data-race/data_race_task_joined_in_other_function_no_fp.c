// SPDX-License-Identifier: Apache-2.0
// One function starts the task's thread and another joins it through the same field of the task.
// The read after the second call comes after the thread has finished.
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

static void stop(struct Task* task)
{
    pthread_join(task->thread, NULL);
}

int main(void)
{
    struct Task task = {0};
    start(&task);
    stop(&task);
    return task.value;
}
