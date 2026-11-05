// SPDX-License-Identifier: Apache-2.0
// main hands writer and task to launch, which hands them on to start; start writes task, then
// starts writer on the pointer it reads out of it, which main pointed at other: writer runs on
// other and takes other's lock, while main takes task's (#242). As start writes task before
// reading the field, what the field holds is not proven, and writer keeps task, two frames up,
// for lack of proof: the lock writer takes is not named after task, which would make main's lock
// seem to exclude writer's access.
// Expected: one data race on shared, main's read against writer's write.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    struct Task* self;
    pthread_t thread;
    int value;
};

static int shared;

static void* writer(void* argument)
{
    struct Task* task = argument;
    pthread_mutex_lock(&task->lock);
    shared += 1;
    pthread_mutex_unlock(&task->lock);
    return NULL;
}

static void start(struct Task* task, void* (*entry)(void*))
{
    task->value = 0;
    pthread_create(&task->thread, NULL, entry, task->self);
}

static void launch(struct Task* task, void* (*entry)(void*))
{
    start(task, entry);
}

int main(void)
{
    struct Task task = {PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0};
    struct Task other = {PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0};
    task.self = &other;
    launch(&task, writer);
    pthread_mutex_lock(&task.lock);
    int seen = shared;
    pthread_mutex_unlock(&task.lock);
    pthread_join(task.thread, NULL);
    return seen;
}
