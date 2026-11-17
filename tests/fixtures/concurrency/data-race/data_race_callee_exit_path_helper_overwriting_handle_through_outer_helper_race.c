// SPDX-License-Identifier: Apache-2.0
// Main hands the object to a function that only calls the restarting callee. On a path of that
// callee that exits, a helper joining on some of its paths only overwrites the handle with another
// thread's, joins it and writes: the write runs beside the thread main started (#201).
// Expected: the write in maybe races with run. The race of run with itself, false (#256), and the
// missing join of the spare thread, joined through the copy of its handle (#255), are not pinned.
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

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

static void maybe(struct Task* task, int really)
{
    if (!really)
        return;
    pthread_t spare;
    pthread_create(&spare, NULL, idle, NULL);
    task->thread = spare;
    pthread_join(task->thread, NULL);
    task->value += 1;
}

static void restart(struct Task* task, int fatal)
{
    if (fatal)
    {
        maybe(task, 1);
        exit(1);
    }
    pthread_join(task->thread, NULL);
    pthread_create(&task->thread, NULL, run, task);
}

static void renew(struct Task* task, int fatal)
{
    restart(task, fatal);
}

int main(int argc, char** argv)
{
    (void)argv;
    struct Task task = {0};
    pthread_create(&task.thread, NULL, run, &task);
    renew(&task, argc > 1);
    pthread_join(task.thread, NULL);
    return task.value;
}
