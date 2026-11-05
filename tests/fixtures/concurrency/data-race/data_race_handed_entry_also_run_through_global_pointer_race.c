// SPDX-License-Identifier: Apache-2.0
// writer increments shared under the lock of the task it is handed. main hands writer and task to
// a helper, which starts writer as a thread on task, and starts reader, which reads shared under
// task's lock. Once the thread is joined, main stores writer's address in a global and runs it
// through it on other, under other's lock, while reader runs. An address kept anywhere but in a
// local slot may be called from anywhere that reads it, on an object no spawn names (#235).
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

static void start(struct Task* started, void* (*entry)(void*))
{
    pthread_create(&started->thread, NULL, entry, started);
}

static void* (*runner)(void*);

int main(void)
{
    pthread_t back;
    start(&task, writer);
    pthread_create(&back, NULL, reader, NULL);
    pthread_join(task.thread, NULL);
    runner = writer;
    runner(&other);
    pthread_join(back, NULL);
    return shared;
}
