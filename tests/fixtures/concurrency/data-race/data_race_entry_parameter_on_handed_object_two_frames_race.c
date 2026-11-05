// SPDX-License-Identifier: Apache-2.0
// The entry and the object reach the spawn through two helpers, in a different order in each: the
// thread still runs on the object main reads (#235).
// Expected: one data race, main against other.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
};

static void* other(void* argument)
{
    struct Task* task = argument;
    task->value += 2;
    return NULL;
}

static void start(struct Task* task, void* (*entry)(void*))
{
    pthread_create(&task->thread, NULL, entry, task);
}

static void launch(void* (*entry)(void*), struct Task* task)
{
    start(task, entry);
}

int main(void)
{
    struct Task task = {0};
    launch(other, &task);
    int seen = task.value;
    pthread_join(task.thread, NULL);
    return seen;
}
