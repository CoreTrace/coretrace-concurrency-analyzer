// SPDX-License-Identifier: Apache-2.0
// first hands the object holding its handle on to a child that replaces the handle: first's own
// accesses show nothing, yet finish may wait for idle and first runs beside second (#201).
// Expected: two data races, first against second and main against first, and the object escaping
// main's frame.
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

static void* child(void* argument)
{
    struct Task* task = argument;
    task->thread = task->spare;
    return NULL;
}

static void* first(void* argument)
{
    pthread_t helper;
    pthread_create(&helper, NULL, child, argument);
    pthread_detach(helper);
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
