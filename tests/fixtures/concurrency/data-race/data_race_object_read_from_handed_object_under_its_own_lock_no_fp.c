// SPDX-License-Identifier: Apache-2.0
// main points the field of task at task or at other, as the run decides, and the helper starts
// writer on what it holds: writer runs on that object and takes its lock (#242). Run on task,
// writer and main take the same lock; run on other, they touch different objects. Nothing proves
// what the field holds, and writer keeps task for lack of proof: its accesses are taken to be
// task's, and the lock around them task's too.
// Expected: no data race.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    struct Task* self;
    pthread_t thread;
    int value;
};

static void* writer(void* argument)
{
    struct Task* task = argument;
    pthread_mutex_lock(&task->lock);
    task->value += 1;
    pthread_mutex_unlock(&task->lock);
    return NULL;
}

static void start(struct Task* task)
{
    pthread_create(&task->thread, NULL, writer, task->self);
}

int main(int argc, char** argv)
{
    (void)argv;
    struct Task task = {PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0};
    struct Task other = {PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0};
    task.self = argc > 1 ? &task : &other;
    start(&task);
    pthread_mutex_lock(&task.lock);
    int seen = task.value;
    pthread_mutex_unlock(&task.lock);
    pthread_join(task.thread, NULL);
    return seen;
}
