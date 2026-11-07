// SPDX-License-Identifier: Apache-2.0
// main hands writer and holder to a helper, which starts writer on the pointer it reads out of
// holder, pointed at task: writer updates task under task's lock, taking first then second there,
// and main does the same around second then first (#242). The store is proven: writer runs on
// task, and the lock it takes is main's, which serializes both the updates and the inversion.
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    int value;
};

struct Holder
{
    struct Task* self;
    pthread_t thread;
};

static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;

static void* writer(void* argument)
{
    struct Task* task = argument;
    pthread_mutex_lock(&task->lock);
    task->value += 1;
    pthread_mutex_lock(&first);
    pthread_mutex_lock(&second);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&task->lock);
    return NULL;
}

static void start(struct Holder* holder, void* (*entry)(void*))
{
    pthread_create(&holder->thread, NULL, entry, holder->self);
}

int main(void)
{
    struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};
    struct Holder holder;
    holder.self = &task;
    start(&holder, writer);
    pthread_mutex_lock(&task.lock);
    task.value -= 1;
    pthread_mutex_lock(&second);
    pthread_mutex_lock(&first);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&task.lock);
    pthread_join(holder.thread, NULL);
    return task.value;
}
