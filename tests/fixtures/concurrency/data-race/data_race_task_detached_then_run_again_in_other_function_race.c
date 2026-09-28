// SPDX-License-Identifier: Apache-2.0
// The owner detaches the task's thread, then calls a function that starts another thread into the
// same field and joins it. That join ends the thread the function started; the detached one is
// still running when the owner reads.
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

static void runOnce(struct Task* task)
{
    start(task);
    stop(task);
}

int main(void)
{
    struct Task task = {0};
    start(&task);
    pthread_detach(task.thread);
    runOnce(&task);
    return task.value;
}
