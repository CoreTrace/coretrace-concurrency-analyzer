// SPDX-License-Identifier: Apache-2.0
// The task's thread field receives a second thread while the first is still running, and only
// the second is joined: the join ends the thread the field holds, not the one it replaced.
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

int main(int argc, char** argv)
{
    (void)argv;
    struct Task task = {0};
    (void)argc;
    start(&task);
    start(&task);
    stop(&task);
    return task.value;
}
