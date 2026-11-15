// SPDX-License-Identifier: Apache-2.0
// writer hands the lock of the object it is handed to helpers taking a lock: the helpers lock
// whatever they are given, here that lock, past the handle's bytes, so main's join waits for writer
// (#201).
// Expected: no data race.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    int value;
    pthread_t thread;
    pthread_mutex_t lock;
};

static struct Task task = {0, 0, PTHREAD_MUTEX_INITIALIZER};
static int shared;

static void acquire(pthread_mutex_t* lock)
{
    pthread_mutex_lock(lock);
}

static void release(pthread_mutex_t* lock)
{
    pthread_mutex_unlock(lock);
}

static void* writer(void* argument)
{
    struct Task* held = argument;
    acquire(&held->lock);
    shared += 1;
    release(&held->lock);
    return NULL;
}

int main(void)
{
    pthread_create(&task.thread, NULL, writer, &task);
    pthread_join(task.thread, NULL);
    return shared;
}
