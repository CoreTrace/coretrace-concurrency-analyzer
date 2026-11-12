// SPDX-License-Identifier: Apache-2.0
// writer replaces its own handle, the first field of the local object it is handed: finish may
// wait for idle, and writer runs past main's return (#201).
// Expected: two data races, finish against writer and main against writer, and the object escaping
// main's frame.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_t thread;
    pthread_t spare;
};

static int shared;

static void* idle(void* argument)
{
    (void)argument;
    return NULL;
}

static void* writer(void* argument)
{
    struct Task* held = argument;
    held->thread = held->spare;
    shared += 1;
    return NULL;
}

static void finish(struct Task* task)
{
    pthread_join(task->thread, NULL);
}

int main(void)
{
    struct Task task = {0, 0};
    pthread_create(&task.spare, NULL, idle, NULL);
    pthread_create(&task.thread, NULL, writer, &task);
    finish(&task);
    pthread_join(task.spare, NULL);
    return shared;
}
