// SPDX-License-Identifier: Apache-2.0
// The function that stops the task joins its thread on one branch only. After the call the thread
// may still be running, so the read races with it.
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

static void stop(struct Task* task, int wait)
{
    if (wait)
        pthread_join(task->thread, NULL);
}

int main(int argc, char** argv)
{
    (void)argv;
    struct Task task = {0};
    start(&task);
    stop(&task, argc > 1);
    return task.value;
}
