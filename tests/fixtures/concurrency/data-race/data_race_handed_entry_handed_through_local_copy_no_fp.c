// SPDX-License-Identifier: Apache-2.0
// writer increments shared under the lock of the task it is handed. main hands a local copy of
// writer, and task, to a helper, which starts writer as a thread on task; reader reads shared under
// task's lock. Every run of writer is that thread's: the copy only carries the address to the
// spawn, which binds writer to task, so both hold task's lock (#235).
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

static void* writer(void* argument)
{
    struct Task* held = argument;
    pthread_mutex_lock(&held->lock);
    shared += 1;
    pthread_mutex_unlock(&held->lock);
    return NULL;
}

static void* reader(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&task.lock);
    int value = shared;
    pthread_mutex_unlock(&task.lock);
    return (void*)(long)value;
}

static void start(struct Task* started, void* (*entry)(void*))
{
    pthread_create(&started->thread, NULL, entry, started);
}

int main(void)
{
    pthread_t back;
    void* (*copy)(void*) = writer;
    start(&task, copy);
    pthread_create(&back, NULL, reader, NULL);
    pthread_join(back, NULL);
    pthread_join(task.thread, NULL);
    return shared;
}
