// SPDX-License-Identifier: Apache-2.0
// A callee starts a thread into the handle before joining it: its join waits for that thread, and
// the one main started, which nothing joins, runs beside it and beside main's read (#201).
// Expected: main races with run, and run with itself; the first thread is never joined and
// outlives the object it is handed.
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

static void restart(struct Task* task)
{
    pthread_create(&task->thread, NULL, run, task);
    pthread_join(task->thread, NULL);
}

int main(void)
{
    struct Task task = {0};
    pthread_create(&task.thread, NULL, run, &task);
    restart(&task);
    return task.value;
}
