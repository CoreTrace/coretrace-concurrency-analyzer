// SPDX-License-Identifier: Apache-2.0
// On a path that exits, the restarting callee publishes the object's address and a thread it starts
// overwrites the handle through it with another thread's. A helper joining on some of its paths
// only then joins the handle and writes: that join waits for the other thread, so the write runs
// beside the thread main started (#201).
// Expected: the write of value in maybe_join races with run, reported where it is made. The race
// of run with itself, false (#256), and the missing join of the spare thread are not pinned.
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

struct Task
{
    int value;
    int rounds;
    pthread_t thread;
};

static struct Task* published;

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

static void* swapper(void* argument)
{
    (void)argument;
    pthread_t spare;
    pthread_create(&spare, NULL, idle, NULL);
    published->thread = spare;
    return NULL;
}

static void maybe_join(struct Task* task, int really)
{
    if (!really)
        return;
    pthread_join(task->thread, NULL);
    task->rounds += 1;
    task->value += 1;
}

static void restart(struct Task* task, int fatal)
{
    if (fatal)
    {
        pthread_t other;
        published = task;
        pthread_create(&other, NULL, swapper, NULL);
        pthread_join(other, NULL);
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
    return task.value;
}
