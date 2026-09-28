// SPDX-License-Identifier: Apache-2.0
// The task arrives running an idle worker. The function stops it through a helper, starts another
// worker into the same field and reads: the stop came before that start and ended only the idle
// worker, so the read races with the new one, joined only afterwards.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
};

static void* idle(void* argument)
{
    (void)argument;
    return NULL;
}

static void* run(void* argument)
{
    struct Task* task = argument;
    task->value += 1;
    return NULL;
}

static void startIdle(struct Task* task)
{
    pthread_create(&task->thread, NULL, idle, NULL);
}

static void start(struct Task* task)
{
    pthread_create(&task->thread, NULL, run, task);
}

static void stop(struct Task* task)
{
    pthread_join(task->thread, NULL);
}

static int restart(struct Task* task)
{
    stop(task);
    start(task);
    const int value = task->value;
    pthread_join(task->thread, NULL);
    return value;
}

int main(void)
{
    struct Task task = {0};
    startIdle(&task);
    return restart(&task);
}
