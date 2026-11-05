// SPDX-License-Identifier: Apache-2.0
// writer increments shared under the lock of the task it is handed. main hands writer and task to
// a helper, which starts writer as a thread on task, and hands writer and other down five helpers
// to the same spawn, which starts a second thread on other; reader reads shared under task's lock.
// The thread on other holds other's lock, which excludes neither the first thread nor reader. The
// calls are followed four helpers up from a spawn: past them, no object is named (#235).
// Expected: two data races on shared, between the two runs of writer and between reader and writer.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

static struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};
static struct Task other = {PTHREAD_MUTEX_INITIALIZER, 0};
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

static void hand4(struct Task* started, void* (*entry)(void*))
{
    hand3(started, entry);
}

int main(void)
{
    pthread_t back;
    start(&task, writer);
    hand4(&other, writer);
    pthread_create(&back, NULL, reader, NULL);
    pthread_join(back, NULL);
    pthread_join(other.thread, NULL);
    pthread_join(task.thread, NULL);
    return shared;
}
