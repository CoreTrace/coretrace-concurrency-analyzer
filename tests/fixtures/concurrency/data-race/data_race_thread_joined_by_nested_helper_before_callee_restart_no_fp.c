// SPDX-License-Identifier: Apache-2.0
// A callee hands the object to a function that joins the thread held in a field and writes, then
// starts another into the field: the join is made while the field still holds the first thread, so
// the write runs beside neither (#201).
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

static void finish(struct Task* task)
{
    pthread_join(task->thread, NULL);
    task->value += 1;
}

static void restart(struct Task* task)
{
    finish(task);
    pthread_create(&task->thread, NULL, run, task);
}

int main(void)
{
    struct Task task = {0};
    pthread_create(&task.thread, NULL, run, &task);
    restart(&task);
    pthread_join(task.thread, NULL);
    return task.value;
}
