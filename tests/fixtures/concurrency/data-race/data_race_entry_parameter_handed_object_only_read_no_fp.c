// SPDX-License-Identifier: Apache-2.0
// The thread started on the handed entry only reads the object it is handed, and so does main
// (#235).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
};

static void* peek(void* argument)
{
    struct Task* task = argument;
    return (void*)(long)task->value;
}

static void start(struct Task* task, void* (*entry)(void*))
{
    pthread_create(&task->thread, NULL, entry, task);
}

int main(void)
{
    struct Task task = {0};
    start(&task, peek);
    int seen = task.value;
    pthread_join(task.thread, NULL);
    return seen;
}
