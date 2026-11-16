// SPDX-License-Identifier: Apache-2.0
// On a path that exits, the restarting callee overwrites the handle with another thread's, then
// calls a helper that joins it on some of its paths only and reads what the thread main started
// writes. The helper changes nothing of the object: the callee's own write is the only change, and
// the helper's join waits for the other thread, so its read runs beside main's thread (#201).
// Expected: the read in maybe_join races with run. The race of run with itself, false (#256), and
// the missing join of the spare thread, joined through its copied handle (#255), are not pinned.
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

struct Task
{
    int value;
    pthread_t thread;
};

static int seen;

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
    seen = task->value;
}

static void restart(struct Task* task, int fatal)
{
    if (fatal)
    {
        pthread_t spare;
        pthread_create(&spare, NULL, idle, NULL);
        task->thread = spare;
        maybe_join(task, 1);
        exit(1);
    }
    pthread_join(task->thread, NULL);
    pthread_create(&task->thread, NULL, run, task);
}

int main(int argc, char** argv)
{
    (void)argv;
    struct Task task = {0};
    pthread_create(&task.thread, NULL, run, &task);
    restart(&task, argc > 1);
    pthread_join(task.thread, NULL);
    return task.value + seen;
}
