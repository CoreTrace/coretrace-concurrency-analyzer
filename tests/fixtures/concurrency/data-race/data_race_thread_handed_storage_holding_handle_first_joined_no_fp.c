// SPDX-License-Identifier: Apache-2.0
// writer is handed the local object whose first field is its handle and writes only the object's
// value; finish joins it through that object. The handle's bytes are the first field's, which the
// value leaves alone, so the join waits for writer before main reads shared and returns (#201).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_t thread;
    int value;
};

static int shared;

static void* writer(void* argument)
{
    struct Task* held = argument;
    held->value = 1;
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
    pthread_create(&task.thread, NULL, writer, &task);
    finish(&task);
    return shared;
}
