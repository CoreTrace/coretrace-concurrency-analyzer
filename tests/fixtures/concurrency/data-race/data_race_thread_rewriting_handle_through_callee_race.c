// SPDX-License-Identifier: Apache-2.0
// first replaces its own handle through a function it calls (#201).
// Expected: at least three data races, first against second, main against first and finish against
// the callee, and the object escaping main's frame. main's join of spare is also reported against
// the call replacing the handle, which writes the handle alone: that one is not pinned.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
    pthread_t spare;
};

static int shared;

static void* idle(void* argument)
{
    (void)argument;
    return NULL;
}

static void* second(void* argument)
{
    (void)argument;
    shared += 2;
    return NULL;
}

static void swap_in_spare(struct Task* task)
{
    task->thread = task->spare;
}

static void* first(void* argument)
{
    swap_in_spare(argument);
    shared += 1;
    return NULL;
}

static void finish(struct Task* task)
{
    pthread_join(task->thread, NULL);
}

int main(void)
{
    struct Task task = {0};
    pthread_t later;
    pthread_create(&task.spare, NULL, idle, NULL);
    pthread_create(&task.thread, NULL, first, &task);
    finish(&task);
    pthread_create(&later, NULL, second, NULL);
    pthread_join(later, NULL);
    pthread_join(task.spare, NULL);
    return shared;
}
