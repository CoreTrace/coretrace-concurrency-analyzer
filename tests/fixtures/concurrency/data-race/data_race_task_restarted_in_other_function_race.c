// SPDX-License-Identifier: Apache-2.0
// Restarting the task joins the thread it runs and starts another into the same field. The join
// comes before that start and ends only the first thread: the second is still running when the
// owner reads.
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

static void restart(struct Task* task)
{
    pthread_join(task->thread, NULL);
    start(task);
}

int main(void)
{
    struct Task task = {0};
    start(&task);
    restart(&task);
    return task.value;
}
