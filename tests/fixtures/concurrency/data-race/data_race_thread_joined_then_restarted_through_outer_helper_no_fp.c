// SPDX-License-Identifier: Apache-2.0
// The same restart, made by a function the callee hands the object on to: the thread main started
// is still joined before the second starts (#201).
// Expected: no diagnostic.
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

static void restart(struct Task* task)
{
    pthread_join(task->thread, NULL);
    pthread_create(&task->thread, NULL, run, task);
}

static void restart_outer(struct Task* task)
{
    restart(task);
}

int main(void)
{
    struct Task task = {0};
    pthread_create(&task.thread, NULL, run, &task);
    restart_outer(&task);
    pthread_join(task.thread, NULL);
    return task.value;
}
