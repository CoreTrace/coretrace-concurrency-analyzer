// SPDX-License-Identifier: Apache-2.0
// writer increments shared under the lock of the task it is handed. main starts reader, which reads
// shared under task's lock, and hands writer and task to a helper, which starts writer as a thread
// on task, joins it, then runs writer on other, under other's lock, while reader runs. The helper
// starting the thread also calls what it is handed: that run has no spawn naming its object (#235).
// Expected: one data race on shared, between reader and writer.
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

static void startThenRun(struct Task* started, void* (*entry)(void*))
{
    pthread_create(&started->thread, NULL, entry, started);
    pthread_join(started->thread, NULL);
    entry(&other);
}

int main(void)
{
    pthread_t back;
    pthread_create(&back, NULL, reader, NULL);
    startThenRun(&task, writer);
    pthread_join(back, NULL);
    return shared;
}
