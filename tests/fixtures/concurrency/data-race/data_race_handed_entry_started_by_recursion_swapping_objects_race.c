// SPDX-License-Identifier: Apache-2.0
// writer increments shared under the lock of the task it is handed. main hands writer, task and
// other to a recursive helper, which hands writer back to itself with the two tasks swapped before
// starting it: one recursion down, the thread runs on other, under other's lock. reader reads
// shared under task's lock, which excludes nothing the thread holds. Handing itself another task,
// the recursion settles the spawn on it too. A helper recursing the same way joins the thread
// (#235).
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

static void start(struct Task* started, struct Task* spare, void* (*entry)(void*), int depth)
{
    if (depth > 0)
    {
        start(spare, started, entry, depth - 1);
        return;
    }
    pthread_create(&started->thread, NULL, entry, started);
}

static void finish(struct Task* started, struct Task* spare, int depth)
{
    if (depth > 0)
    {
        finish(spare, started, depth - 1);
        return;
    }
    pthread_join(started->thread, NULL);
}

int main(void)
{
    pthread_t back;
    start(&task, &other, writer, 1);
    pthread_create(&back, NULL, reader, NULL);
    pthread_join(back, NULL);
    finish(&task, &other, 1);
    return 0;
}
