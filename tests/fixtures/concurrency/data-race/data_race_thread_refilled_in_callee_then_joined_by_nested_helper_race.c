// SPDX-License-Identifier: Apache-2.0
// A helper starts a thread on a handle. A callee then starts another on that handle and hands it to
// a function that joins it and writes: that join waits for the second thread, and the first, which
// nothing joins, still runs beside the write.
// Expected: the write in finish races with run, and run with itself.
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

static void start(struct Task* task)
{
    pthread_create(&task->thread, NULL, run, task);
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
    start(&task);
    refill_then_finish(&task);
    return 0;
}
