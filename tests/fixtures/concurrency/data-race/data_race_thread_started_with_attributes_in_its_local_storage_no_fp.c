// SPDX-License-Identifier: Apache-2.0
// main starts reader with the attributes stored beside its handle in a local object and hands it
// nothing: pthread_create only reads the attributes, so reader cannot replace its handle, and the
// join waits for it before main writes shared (#201).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_attr_t attributes;
    pthread_t thread;
};

static int shared;

static void* reader(void* argument)
{
    (void)argument;
    return (void*)(long)shared;
}

int main(void)
{
    struct Task task;
    pthread_attr_init(&task.attributes);
    pthread_create(&task.thread, &task.attributes, reader, NULL);
    pthread_join(task.thread, NULL);
    pthread_attr_destroy(&task.attributes);
    shared = 1;
    return shared;
}
