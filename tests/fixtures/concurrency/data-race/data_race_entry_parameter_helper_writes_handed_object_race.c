// SPDX-License-Identifier: Apache-2.0
// The helper writes the object it handed to the thread it started on the handed entry, before
// joining it: the thread is not one left running by the call, yet it reaches the object (#235).
// Expected: one data race, runWith against add.
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

static void runWith(struct Task* task, void* (*entry)(void*))
{
    pthread_create(&task->thread, NULL, entry, task);
    task->value = 1;
    pthread_join(task->thread, NULL);
}

int main(void)
{
    struct Task task = {0};
    runWith(&task, add);
    return task.value;
}
