// SPDX-License-Identifier: Apache-2.0
// Two launchers each hand the same helper an entry of their own and the object they are handed:
// add runs on first and peek on second, settled a frame above the spawn (#235).
// Expected: two data races, main against add on first and main against peek on second.
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

static void* peek(void* argument)
{
    struct Task* task = argument;
    return (void*)(long)task->value;
}

static void start(struct Task* task, void* (*entry)(void*))
{
    pthread_create(&task->thread, NULL, entry, task);
}

static void launchAdd(struct Task* task)
{
    start(task, add);
}

static void launchPeek(struct Task* task)
{
    start(task, peek);
}

int main(void)
{
    struct Task first = {0};
    struct Task second = {0};
    launchAdd(&first);
    launchPeek(&second);
    int seen = first.value;
    second.value = 1;
    pthread_join(first.thread, NULL);
    pthread_join(second.thread, NULL);
    return seen;
}
