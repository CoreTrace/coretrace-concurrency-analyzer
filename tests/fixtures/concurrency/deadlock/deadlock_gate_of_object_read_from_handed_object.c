// SPDX-License-Identifier: Apache-2.0
// main hands writer and task to a helper, which starts writer on the pointer it reads out of
// task, pointed at other: writer takes other's lock around first then second, while main takes
// task's around second then first (#242). The two gates differ, and the inversion may deadlock.
// main also hands task's address to pthread_mutex_lock, so what the field holds is not proven,
// and writer keeps task for lack of proof: the lock it takes is not a gate named after task,
// which would make main's lock seem to serialize the inversion.
// Expected: one deadlock, writer taking first then second and main second then first.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    struct Task* self;
    pthread_mutex_t lock;
    pthread_t thread;
};

static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;

static void* writer(void* argument)
{
    struct Task* task = argument;
    pthread_mutex_lock(&task->lock);
    pthread_mutex_lock(&first);
    pthread_mutex_lock(&second);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&task->lock);
    return NULL;
}

static void start(struct Task* task, void* (*entry)(void*))
{
    pthread_create(&task->thread, NULL, entry, task->self);
}

int main(void)
{
    struct Task task = {NULL, PTHREAD_MUTEX_INITIALIZER, 0};
    struct Task other = {NULL, PTHREAD_MUTEX_INITIALIZER, 0};
    task.self = &other;
    start(&task, writer);
    pthread_mutex_lock(&task.lock);
    pthread_mutex_lock(&second);
    pthread_mutex_lock(&first);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&task.lock);
    pthread_join(task.thread, NULL);
    return 0;
}
