// SPDX-License-Identifier: Apache-2.0
// A callee joins the thread main started on one path only before starting another into its handle:
// on the other path the first thread, which nothing joins, runs beside the second and beside main's
// read (#201).
// Expected: main races with run, and run with itself.
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

static void restart(struct Task* task, int wait)
{
    if (wait)
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
