// SPDX-License-Identifier: Apache-2.0
// A callee swaps the handle with another thread's before joining it: its join waits for that other
// thread, so the one main started still runs beside the thread the callee starts, and beside main's
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

static void* idle(void* argument)
{
    (void)argument;
    return NULL;
}

static void restart(struct Task* task, pthread_t* spare)
{
    pthread_t held = task->thread;
    task->thread = *spare;
    *spare = held;
    pthread_join(task->thread, NULL);
    pthread_create(&task->thread, NULL, run, task);
}

int main(void)
{
    struct Task task = {0};
    pthread_t spare;
    pthread_create(&spare, NULL, idle, NULL);
    pthread_create(&task.thread, NULL, run, &task);
    restart(&task, &spare);
    pthread_join(task.thread, NULL);
    int seen = task.value;
    pthread_join(spare, NULL);
    return seen;
}
