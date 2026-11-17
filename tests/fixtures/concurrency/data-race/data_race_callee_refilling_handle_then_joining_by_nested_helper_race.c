// SPDX-License-Identifier: Apache-2.0
// A callee starts a thread into the handle, then hands the object to a function that joins the
// handle and writes: that join waits for the second thread, and the one main started, which nothing
// joins, still runs beside the write (#201).
// Expected: the write in finish races with run, and run with itself; the first thread is never
// joined and outlives the object it is handed.
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

static void finish(struct Task* task)
{
    pthread_join(task->thread, NULL);
    task->value += 1;
}

static void refill_then_finish(struct Task* task)
{
    pthread_create(&task->thread, NULL, run, task);
    finish(task);
}

int main(void)
{
    struct Task task = {0};
    pthread_create(&task.thread, NULL, run, &task);
    refill_then_finish(&task);
    return 0;
}
