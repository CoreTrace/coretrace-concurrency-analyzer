// SPDX-License-Identifier: Apache-2.0
// The helper starts writer on the pointer it reads out of task, which main pointed at other:
// writer runs on other and takes other's lock, while main takes task's (#242). main also hands
// task's address to pthread_mutex_lock, so what the field holds is not proven, and writer keeps
// task for lack of proof: the lock writer takes is not named after task, which would make main's
// lock seem to exclude writer's access.
// Expected: one data race on shared, main's read against writer's write.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    struct Task* self;
    pthread_t thread;
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

static void start(struct Task* task)
{
    pthread_create(&task->thread, NULL, writer, task->self);
}

int main(void)
{
    struct Task task = {PTHREAD_MUTEX_INITIALIZER, NULL, 0};
    struct Task other = {PTHREAD_MUTEX_INITIALIZER, NULL, 0};
    task.self = &other;
    start(&task);
    pthread_mutex_lock(&task.lock);
    int seen = shared;
    pthread_mutex_unlock(&task.lock);
    pthread_join(task.thread, NULL);
    return seen;
}
