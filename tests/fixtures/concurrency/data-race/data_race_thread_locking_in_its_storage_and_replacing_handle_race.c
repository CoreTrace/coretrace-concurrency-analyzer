// SPDX-License-Identifier: Apache-2.0
// first takes the lock of the object it is handed and, under it, replaces its own handle there:
// the lock's bytes are not the handle's, but the store is, so finish may wait for idle and first
// runs beside second and past main's return (#201).
// Expected: three data races, first against second, finish against first and main against first,
// and the object escaping main's frame.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
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
    pthread_mutex_lock(&task->lock);
    task->thread = task->spare;
    pthread_mutex_unlock(&task->lock);
    shared += 1;
    return NULL;
}

static void finish(struct Task* task)
{
    pthread_join(task->thread, NULL);
}

int main(void)
{
    struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0, 0};
    pthread_t later;
    pthread_create(&task.spare, NULL, idle, NULL);
    pthread_create(&task.thread, NULL, first, &task);
    finish(&task);
    pthread_create(&later, NULL, second, NULL);
    pthread_join(later, NULL);
    pthread_join(task.spare, NULL);
    return shared;
}
