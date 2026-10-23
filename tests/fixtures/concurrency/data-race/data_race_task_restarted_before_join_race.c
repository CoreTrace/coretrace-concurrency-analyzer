// SPDX-License-Identifier: Apache-2.0
// restart starts other into the task's handle before joining it: the join waits for other, and
// run, started by main and never joined, still runs when main reads the value. The callee's join
// is no join of the thread its caller handed it unless nothing changed the handle before it
// (#213, #201).
// Expected: two data races, main against run and other against run, one missing join and one
// thread argument escaping its frame, for run.
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

static void* other(void* argument)
{
    struct Task* task = argument;
    task->value += 2;
    return NULL;
}

static void restart(struct Task* task)
{
    pthread_create(&task->thread, NULL, other, task);
    pthread_join(task->thread, NULL);
}

int main(void)
{
    struct Task task = {0};
    pthread_create(&task.thread, NULL, run, &task);
    restart(&task);
    return task.value;
}
