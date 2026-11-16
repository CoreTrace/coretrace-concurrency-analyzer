// SPDX-License-Identifier: Apache-2.0
// The restarting callee joins the thread main started, then on one path writes what that thread
// wrote and exits: the thread has ended by then. On the other path it starts another, which main
// joins before reading (#201).
// Expected: no diagnostic.
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

static void restart(struct Task* task, int fatal)
{
    pthread_join(task->thread, NULL);
    if (fatal)
    {
        task->value = 0;
        exit(1);
    }
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
