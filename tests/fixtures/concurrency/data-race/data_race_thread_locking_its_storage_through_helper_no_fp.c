// SPDX-License-Identifier: Apache-2.0
// writer takes the lock of the object it is handed through helpers given the whole object, which
// also holds its handle: what the helpers change is the lock, and main's join waits for writer
// (#201).
// Expected: no data race.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

static struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};
static int shared;

static void lock_task(struct Task* locked)
{
    pthread_mutex_lock(&locked->lock);
}

static void unlock_task(struct Task* locked)
{
    pthread_mutex_unlock(&locked->lock);
}

static void* writer(void* argument)
{
    struct Task* held = argument;
    lock_task(held);
    shared += 1;
    unlock_task(held);
    return NULL;
}

int main(void)
{
    pthread_create(&task.thread, NULL, writer, &task);
    pthread_join(task.thread, NULL);
    return shared;
}
