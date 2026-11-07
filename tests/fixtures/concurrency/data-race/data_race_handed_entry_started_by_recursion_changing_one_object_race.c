// SPDX-License-Identifier: Apache-2.0
// writer increments shared under the lock of the task it is handed. A recursive helper starts
// writer on its first task, joins it, then starts it on its second; it hands itself the first task
// unchanged but spare as the second, so one recursion down the second thread runs on spare. main
// hands task as both tasks, down three helpers to the recursion. reader reads shared under task's
// lock, which excludes nothing the thread on spare holds. The recursion hands the entry back as it
// found it for the first spawn only: for the second, it is one helper more past those followed. A
// helper recursing the same way joins the second thread (#235).
// Expected: one data race on shared, between reader and writer.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

static struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};
static struct Task spare = {PTHREAD_MUTEX_INITIALIZER, 0};
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

static void start(struct Task* first, struct Task* second, void* (*entry)(void*), int depth)
{
    if (depth > 0)
    {
        start(first, &spare, entry, depth - 1);
        return;
    }
    pthread_create(&first->thread, NULL, entry, first);
    pthread_join(first->thread, NULL);
    pthread_create(&second->thread, NULL, entry, second);
}

static void finish(struct Task* second, int depth)
{
    if (depth > 0)
    {
        finish(&spare, depth - 1);
        return;
    }
    pthread_join(second->thread, NULL);
}

static void hand1(struct Task* first, struct Task* second, void* (*entry)(void*))
{
    start(first, second, entry, 1);
}

static void hand2(struct Task* first, struct Task* second, void* (*entry)(void*))
{
    hand1(first, second, entry);
}

static void hand3(struct Task* first, struct Task* second, void* (*entry)(void*))
{
    hand2(first, second, entry);
}

int main(void)
{
    pthread_t back;
    hand3(&task, &task, writer);
    pthread_create(&back, NULL, reader, NULL);
    pthread_join(back, NULL);
    finish(&task, 1);
    return 0;
}
