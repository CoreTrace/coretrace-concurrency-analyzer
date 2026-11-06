// SPDX-License-Identifier: Apache-2.0
// writer increments shared under the lock of the task it is handed. A recursive helper starts
// writer on the task the one it is handed points to, and hands itself that task in turn. main's
// call names middle, the task top points to, but one recursion down the thread runs on deep, the
// task middle points to. reader reads shared under middle's lock, which excludes nothing the thread
// holds. The recursion hands itself what it read, which no proof settles (#235).
// Expected: one data race on shared, between reader and writer.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    struct Task* inner;
};

static struct Task deep = {PTHREAD_MUTEX_INITIALIZER, NULL};
static struct Task middle = {PTHREAD_MUTEX_INITIALIZER, &deep};
static pthread_t thread;
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
    pthread_mutex_lock(&middle.lock);
    int value = shared;
    pthread_mutex_unlock(&middle.lock);
    return (void*)(long)value;
}

static void start(struct Task* outer, void* (*entry)(void*), int depth)
{
    if (depth > 0)
    {
        start(outer->inner, entry, depth - 1);
        return;
    }
    pthread_create(&thread, NULL, entry, outer->inner);
}

int main(void)
{
    struct Task top = {PTHREAD_MUTEX_INITIALIZER, NULL};
    pthread_t back;
    top.inner = &middle;
    start(&top, writer, 1);
    pthread_create(&back, NULL, reader, NULL);
    pthread_join(back, NULL);
    pthread_join(thread, NULL);
    return 0;
}
