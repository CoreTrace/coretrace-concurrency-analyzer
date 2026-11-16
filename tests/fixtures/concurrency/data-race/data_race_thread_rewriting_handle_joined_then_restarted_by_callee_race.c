// SPDX-License-Identifier: Apache-2.0
// The thread main starts overwrites its own handle with another thread's. A callee then joins the
// handle and starts run into it: its join may wait for the other thread, so the first may still run
// beside run and beside main's read (#201).
// Expected: main races with rewriting on the handle and on value.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
    pthread_t spare;
};

static void* idle(void* argument)
{
    (void)argument;
    return NULL;
}

static void* run(void* argument)
{
    struct Task* task = argument;
    task->value += 1;
    return NULL;
}

static void* rewriting(void* argument)
{
    struct Task* task = argument;
    task->thread = task->spare;
    task->value += 1;
    return NULL;
}

static void restart(struct Task* task)
{
    pthread_join(task->thread, NULL);
    pthread_create(&task->thread, NULL, run, task);
}

int main(void)
{
    struct Task task = {0};
    pthread_create(&task.spare, NULL, idle, NULL);
    pthread_create(&task.thread, NULL, rewriting, &task);
    restart(&task);
    pthread_join(task.thread, NULL);
    int seen = task.value;
    pthread_join(task.spare, NULL);
    return seen;
}
