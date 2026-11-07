// SPDX-License-Identifier: Apache-2.0
// One helper is called twice, handed a different entry and a different object each time: add
// writes first while peek reads second. main reads first and writes second while both run, so
// each entry keeps the object its own call handed it (#235).
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

int main(void)
{
    struct Task first = {0};
    struct Task second = {0};
    start(&first, add);
    start(&second, peek);
    int seen = first.value;
    second.value = 1;
    pthread_join(first.thread, NULL);
    pthread_join(second.thread, NULL);
    return seen;
}
