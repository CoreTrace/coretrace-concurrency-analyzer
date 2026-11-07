// SPDX-License-Identifier: Apache-2.0
// A loop calls the helper twice with the same entry and the same object: the two threads write it
// together, and main reads it before joining them (#235).
// Expected: two data races, add against itself and main against add.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
};

static void* add(void* argument)
{
    struct Task* task = argument;
    task->value += 2;
    return NULL;
}

static void start(pthread_t* thread, struct Task* task, void* (*entry)(void*))
{
    pthread_create(thread, NULL, entry, task);
}

int main(void)
{
    struct Task task = {0};
    pthread_t threads[2];
    for (int i = 0; i < 2; ++i)
        start(&threads[i], &task, add);
    int seen = task.value;
    for (int i = 0; i < 2; ++i)
        pthread_join(threads[i], NULL);
    return seen;
}
