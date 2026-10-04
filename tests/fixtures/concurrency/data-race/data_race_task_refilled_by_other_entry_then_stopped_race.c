// SPDX-License-Identifier: Apache-2.0
// The task's field receives a run thread, then an idle thread while run still runs. stop() joins
// the field, which ends the idle thread only, then reads the task: run is still running there
// (#126).
// Expected: one data race, stop's read against run.
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

static void* idle(void* argument)
{
    (void)argument;
    return NULL;
}

static void start(struct Task* task)
{
    pthread_create(&task->thread, NULL, run, task);
}

static void startIdle(struct Task* task)
{
    pthread_create(&task->thread, NULL, idle, task);
}

static int stop(struct Task* task)
{
    pthread_join(task->thread, NULL);
    return task->value;
}

int main(void)
{
    struct Task task = {0};
    start(&task);
    startIdle(&task);
    return stop(&task);
}
