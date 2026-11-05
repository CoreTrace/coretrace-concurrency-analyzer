// SPDX-License-Identifier: Apache-2.0
// writer increments shared under the lock of the task it is handed. main hands task down three
// helpers to a fourth, which hands writer to a helper starting it as a thread on task; reader reads
// shared under task's lock. The object is followed four calls up from where writer is handed: it
// is task, and both threads hold task's lock (#235).
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

static void startWriter(struct Task* started)
{
    start(started, writer);
}

static void hand1(struct Task* started)
{
    startWriter(started);
}

static void hand2(struct Task* started)
{
    hand1(started);
}

static void hand3(struct Task* started)
{
    hand2(started);
}

int main(void)
{
    pthread_t back;
    hand3(&task);
    pthread_create(&back, NULL, reader, NULL);
    pthread_join(back, NULL);
    pthread_join(task.thread, NULL);
    return 0;
}
