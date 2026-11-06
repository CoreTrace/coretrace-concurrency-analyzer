// SPDX-License-Identifier: Apache-2.0
// writer increments shared under the lock of the task it is handed. main hands writer and task down
// two helpers to a spawn, which starts writer as a thread on task, joins it, then hands them down
// four helpers to the same spawn, which starts writer on task again; reader reads shared under
// task's lock. Both ways down stay within the four helpers followed up from a spawn: writer is
// bound to task, and every thread holds task's lock (#235).
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

static void hand1(struct Task* started, void* (*entry)(void*))
{
    start(started, entry);
}

static void hand2(struct Task* started, void* (*entry)(void*))
{
    hand1(started, entry);
}

static void hand3(struct Task* started, void* (*entry)(void*))
{
    hand2(started, entry);
}

int main(void)
{
    pthread_t back;
    hand1(&task, writer);
    pthread_join(task.thread, NULL);
    hand3(&task, writer);
    pthread_create(&back, NULL, reader, NULL);
    pthread_join(back, NULL);
    pthread_join(task.thread, NULL);
    return shared;
}
