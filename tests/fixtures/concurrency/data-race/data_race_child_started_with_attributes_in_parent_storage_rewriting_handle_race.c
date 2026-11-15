// SPDX-License-Identifier: Apache-2.0
// first starts a child with the attributes stored beside its handle and hands it that object,
// where the child replaces first's handle. The attributes hand the child nothing, its argument
// does: finish may wait for idle and first runs beside second (#201).
// Expected: two data races, first against second and main against first, and the object escaping
// main's frame.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_attr_t attributes;
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
    struct Task* task = argument;
    pthread_t helper;
    pthread_create(&helper, &task->attributes, child, argument);
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
    struct Task task;
    pthread_t later;
    pthread_attr_init(&task.attributes);
    pthread_create(&task.spare, NULL, idle, NULL);
    pthread_create(&task.thread, NULL, first, &task);
    finish(&task);
    pthread_create(&later, NULL, second, NULL);
    pthread_join(later, NULL);
    pthread_join(task.spare, NULL);
    pthread_attr_destroy(&task.attributes);
    return shared;
}
