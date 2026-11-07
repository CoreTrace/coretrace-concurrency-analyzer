// SPDX-License-Identifier: Apache-2.0
// lockAndOrder takes the lock of the task it is handed, updates the task, then takes first then
// second, and writer calls it on its own task. main hands writer and a holder pointing at task to
// a helper, which starts writer on the pointer it reads out of the holder, while main takes
// task's lock, updates task, then takes second then first (#242). The store is proven: the thread
// holds task, and the lock its call to lockAndOrder takes is main's, a gate both inversions hold.
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

static void lockAndOrder(struct Task* task)
{
    pthread_mutex_lock(&task->lock);
    task->value += 1;
    pthread_mutex_lock(&first);
    pthread_mutex_lock(&second);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&task->lock);
}

static void* writer(void* argument)
{
    lockAndOrder(argument);
    return NULL;
}

static void start(struct Holder* holder, void* (*entry)(void*))
{
    pthread_create(&holder->thread, NULL, entry, holder->self);
}

int main(void)
{
    struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};
    struct Holder holder = {NULL, 0};
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
