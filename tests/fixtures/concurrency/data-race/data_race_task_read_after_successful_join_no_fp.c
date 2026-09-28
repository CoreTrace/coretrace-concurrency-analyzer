// SPDX-License-Identifier: Apache-2.0
// The control for data_race_task_read_after_failed_join_race.c: the read is on the branch where
// the join succeeded, after the thread has finished.
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
    if (pthread_join(task.thread, NULL) == 0)
        return task.value;
    return 0;
}
