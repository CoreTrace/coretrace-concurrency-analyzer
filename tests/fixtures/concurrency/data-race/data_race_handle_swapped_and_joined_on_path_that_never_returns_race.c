// SPDX-License-Identifier: Apache-2.0
// On a path that exits, a function overwrites the handle with another thread's, joins it and
// writes: that join waits for the other thread, so the write runs beside the thread main started.
// On the other path a callee joins main's thread and starts another, which main joins (#201).
// Expected: the write in swap_and_join races with run. The missing join reported for the spare
// thread, joined through the copy of its handle (#255), is not pinned.
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

static void swap_and_join(struct Task* task)
{
    pthread_t spare;
    pthread_create(&spare, NULL, idle, NULL);
    task->thread = spare;
    pthread_join(task->thread, NULL);
    task->value += 1;
}

static void restart(struct Task* task)
{
    pthread_join(task->thread, NULL);
    pthread_create(&task->thread, NULL, run, task);
}

int main(int argc, char** argv)
{
    (void)argv;
    struct Task task = {0};
    pthread_create(&task.thread, NULL, run, &task);
    if (argc > 1)
    {
        swap_and_join(&task);
        exit(1);
    }
    restart(&task);
    pthread_join(task.thread, NULL);
    return task.value;
}
