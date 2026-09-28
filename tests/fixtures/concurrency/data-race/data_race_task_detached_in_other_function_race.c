// SPDX-License-Identifier: Apache-2.0
// The control for data_race_task_joined_in_other_function_no_fp.c: the second function detaches
// the thread instead of joining it, so the read may still race with it.
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
    pthread_detach(task->thread);
}

int main(void)
{
    struct Task task = {0};
    start(&task);
    stop(&task);
    return task.value;
}
