// SPDX-License-Identifier: Apache-2.0
// writer writes the object's value under its lock, while main reads the value before joining
// without that lock: the lock's bytes are synchronization, but the value is data, and both
// accesses still race (#201).
// Expected: one data race, main's read against writer's write.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
    int value;
};

static struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0, 0};

static void* writer(void* argument)
{
    struct Task* held = argument;
    pthread_mutex_lock(&held->lock);
    held->value = 1;
    pthread_mutex_unlock(&held->lock);
    return NULL;
}

int main(void)
{
    pthread_create(&task.thread, NULL, writer, &task);
    int seen = task.value;
    pthread_join(task.thread, NULL);
    return seen;
}
