// SPDX-License-Identifier: Apache-2.0
// The function that stops the task reports a failed join and returns anyway. A failed join has
// waited for nothing, so after the call the thread may still be running and the read races with
// it.
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>

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
    if (pthread_join(task->thread, NULL) != 0)
        perror("pthread_join");
}

int main(void)
{
    struct Task task = {0};
    start(&task);
    stop(&task);
    return task.value;
}
