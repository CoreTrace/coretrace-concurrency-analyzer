// SPDX-License-Identifier: Apache-2.0
// The helper joins the thread it started on the handed entry before returning: main reads the
// object only once that thread has ended (#235).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
};

static void* add(void* argument)
{
    struct Task* task = argument;
    task->value += 2;
    return NULL;
}

static void runToEnd(struct Task* task, void* (*entry)(void*))
{
    pthread_create(&task->thread, NULL, entry, task);
    pthread_join(task->thread, NULL);
}

int main(void)
{
    struct Task task = {0};
    runToEnd(&task, add);
    return task.value;
}
