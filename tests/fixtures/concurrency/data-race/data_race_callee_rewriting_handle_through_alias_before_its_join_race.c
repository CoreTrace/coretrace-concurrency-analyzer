// SPDX-License-Identifier: Apache-2.0
// A callee overwrites the handle through a second pointer to the object before joining it through
// the first: its join waits for the other thread, so the one main started still runs beside the
// thread the callee starts, and beside main's read (#201).
// Expected: main races with run, run with itself. The spare's missing join (#255) is not pinned.
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

static void* idle(void* argument)
{
    (void)argument;
    return NULL;
}

static void restart(struct Task* task, struct Task* alias)
{
    pthread_t spare;
    pthread_create(&spare, NULL, idle, NULL);
    alias->thread = spare;
    pthread_join(task->thread, NULL);
    pthread_create(&task->thread, NULL, run, task);
}

int main(void)
{
    struct Task task = {0};
    pthread_create(&task.thread, NULL, run, &task);
    restart(&task, &task);
    pthread_join(task.thread, NULL);
    return task.value;
}
