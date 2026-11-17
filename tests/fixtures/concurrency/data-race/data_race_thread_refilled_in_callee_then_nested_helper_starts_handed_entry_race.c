// SPDX-License-Identifier: Apache-2.0
// A helper starts run on a handle. A callee then starts an idle thread on that handle and hands it
// to a function that joins it, then starts the entry it is handed, run: that join waits for the idle
// thread, so the first run, which nothing joins, still runs beside the second.
// Expected: run races with itself.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
    pthread_t other;
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

static void start(struct Task* task)
{
    pthread_create(&task->thread, NULL, run, task);
}

static void join_then_start(struct Task* task, void* (*entry)(void*))
{
    pthread_join(task->thread, NULL);
    pthread_create(&task->other, NULL, entry, task);
}

static void refill_then_again(struct Task* task)
{
    pthread_create(&task->thread, NULL, idle, NULL);
    join_then_start(task, run);
}

int main(void)
{
    struct Task task = {0};
    start(&task);
    refill_then_again(&task);
    pthread_join(task.other, NULL);
    return 0;
}
