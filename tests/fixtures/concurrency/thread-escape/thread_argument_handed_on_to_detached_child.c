// SPDX-License-Identifier: Apache-2.0
// first is handed the object holding its own handle and hands it on to a detached child that reads
// it: finish joins first, yet the child may still read main's local after main returns. That first
// has ended says nothing of the threads it handed the object to (#201).
// Expected: the object escaping main's frame.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
};

static void* child(void* argument)
{
    struct Task* task = argument;
    return (void*)(long)task->value;
}

static void* first(void* argument)
{
    pthread_t helper;
    pthread_create(&helper, NULL, child, argument);
    pthread_detach(helper);
    return NULL;
}

static void finish(struct Task* task)
{
    pthread_join(task->thread, NULL);
}

int main(void)
{
    struct Task task = {0};
    pthread_create(&task.thread, NULL, first, &task);
    finish(&task);
    return 0;
}
