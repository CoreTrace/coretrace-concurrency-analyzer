// SPDX-License-Identifier: Apache-2.0
// The helper starts a thread on the entry its caller hands it, giving that thread the object it is
// also handed: main reads the object while the thread writes it (#235).
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

int main(void)
{
    struct Task task = {0};
    start(&task, other);
    int seen = task.value;
    pthread_join(task.thread, NULL);
    return seen;
}
