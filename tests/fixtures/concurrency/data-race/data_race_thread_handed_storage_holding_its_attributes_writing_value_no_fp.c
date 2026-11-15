// SPDX-License-Identifier: Apache-2.0
// writer is started with the attributes stored beside its handle and handed that object, of which
// it writes only the value. The attributes hand it nothing and its own write leaves the handle
// alone, so the join waits for writer before main destroys the attributes and reads shared (#201).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_attr_t attributes;
    pthread_t thread;
    int value;
};

static struct Task task;
static int shared;

static void* writer(void* argument)
{
    struct Task* held = argument;
    held->value = 1;
    shared += 1;
    return NULL;
}

int main(void)
{
    pthread_attr_init(&task.attributes);
    pthread_create(&task.thread, &task.attributes, writer, &task);
    pthread_join(task.thread, NULL);
    pthread_attr_destroy(&task.attributes);
    return shared;
}
