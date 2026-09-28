// SPDX-License-Identifier: Apache-2.0
// A function receives the task with its thread running, starts another into the same field and
// joins that one. The join ends the thread the field now holds, not the one it replaced, which is
// still running when the function reads.
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

static int rerun(struct Task* task)
{
    start(task);
    stop(task);
    return task->value;
}

int main(void)
{
    struct Task task = {0};
    start(&task);
    return rerun(&task);
}
