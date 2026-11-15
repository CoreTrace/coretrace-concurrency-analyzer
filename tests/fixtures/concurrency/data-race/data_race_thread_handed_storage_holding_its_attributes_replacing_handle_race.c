// SPDX-License-Identifier: Apache-2.0
// first is started with the attributes stored beside its handle and handed that object, where it
// replaces its own handle. The attributes change nothing to that: finish may wait for idle, and
// first runs beside second and past main's return (#201).
// Expected: three data races, first against second, finish against first and main against first,
// and the object escaping main's frame.
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

static void* first(void* argument)
{
    struct Task* task = argument;
    task->thread = task->spare;
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
    pthread_create(&task.thread, &task.attributes, first, &task);
    finish(&task);
    pthread_create(&later, NULL, second, NULL);
    pthread_join(later, NULL);
    pthread_join(task.spare, NULL);
    pthread_attr_destroy(&task.attributes);
    return shared;
}
