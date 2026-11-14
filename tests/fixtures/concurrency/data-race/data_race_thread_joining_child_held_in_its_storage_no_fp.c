// SPDX-License-Identifier: Apache-2.0
// writer is handed the object holding its handle, starts a child whose handle it keeps there, and
// joins the child by that handle's value. The value names the child to the runtime, whatever type
// the C library gives it, and designates nothing of the object: writer's own handle stays, and the
// join waits for writer before main reads shared (#201).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
    pthread_t child;
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
    pthread_create(&held->child, NULL, leaf, NULL);
    pthread_join(held->child, NULL);
    shared += 1;
    return NULL;
}

int main(void)
{
    pthread_create(&task.thread, NULL, writer, &task);
    pthread_join(task.thread, NULL);
    return shared;
}
