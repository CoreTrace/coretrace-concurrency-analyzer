// SPDX-License-Identifier: Apache-2.0
// A recursive helper hands the entry and the object on to itself before starting the thread: the
// call from main still settles both, however deep the recursion, and settling them ends although
// the helper keeps handing the entry on to itself (#235).
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

static void start(struct Task* task, void* (*entry)(void*), int depth)
{
    if (depth > 0)
    {
        start(task, entry, depth - 1);
        return;
    }
    pthread_create(&task->thread, NULL, entry, task);
}

int main(void)
{
    struct Task task = {0};
    start(&task, other, 2);
    int seen = task.value;
    pthread_join(task.thread, NULL);
    return seen;
}
