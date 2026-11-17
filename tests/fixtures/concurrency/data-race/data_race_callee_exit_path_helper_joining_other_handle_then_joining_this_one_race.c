// SPDX-License-Identifier: Apache-2.0
// The object holds two handles. On a path of the restarting callee that exits, a function joins
// the other one, overwrites the handle of the thread main started, and itself calls a helper
// joining that handle on some of its paths only, which then writes. Everything past the function's
// own join lies inside it, and that join waited for another thread: the helper's write runs beside
// the thread main started (#201).
// Expected: the write in maybe_join races with run. The race of run with itself, false (#256), and
// the missing join of the spare thread, joined through its copied handle (#255), are not pinned.
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

struct Task
{
    int value;
    pthread_t thread;
    pthread_t other;
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

static void maybe_join(struct Task* task, int really)
{
    if (!really)
        return;
    pthread_join(task->thread, NULL);
    task->value += 1;
}

static void park(struct Task* task)
{
    pthread_t spare;
    pthread_join(task->other, NULL);
    pthread_create(&spare, NULL, idle, NULL);
    task->thread = spare;
    maybe_join(task, 1);
}

static void restart(struct Task* task, int fatal)
{
    if (fatal)
    {
        park(task);
        exit(1);
    }
    pthread_join(task->thread, NULL);
    pthread_create(&task->thread, NULL, run, task);
}

int main(int argc, char** argv)
{
    (void)argv;
    struct Task task = {0};
    pthread_create(&task.other, NULL, idle, NULL);
    pthread_create(&task.thread, NULL, run, &task);
    restart(&task, argc > 1);
    pthread_join(task.thread, NULL);
    pthread_join(task.other, NULL);
    return task.value;
}
