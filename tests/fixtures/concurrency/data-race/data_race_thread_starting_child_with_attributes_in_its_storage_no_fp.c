// SPDX-License-Identifier: Apache-2.0
// writer is handed the object holding its handle and starts a child with the attributes stored
// there, handing the child nothing. pthread_create only reads the attributes, so neither thread
// can replace writer's handle, and the join waits for writer before main reads shared (#201).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_attr_t attributes;
    pthread_t thread;
};

static struct Task task;
static int shared;

static void* leaf(void* argument)
{
    (void)argument;
    return NULL;
}

static void* writer(void* argument)
{
    struct Task* held = argument;
    pthread_t child;
    pthread_create(&child, &held->attributes, leaf, NULL);
    pthread_join(child, NULL);
    shared += 1;
    return NULL;
}

int main(void)
{
    pthread_attr_init(&task.attributes);
    pthread_create(&task.thread, NULL, writer, &task);
    pthread_join(task.thread, NULL);
    pthread_attr_destroy(&task.attributes);
    return shared;
}
