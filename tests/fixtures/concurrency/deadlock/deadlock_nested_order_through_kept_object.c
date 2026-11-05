// SPDX-License-Identifier: Apache-2.0
// writer holds the lock of the task it is handed while it calls takeGate, which takes gate. A
// helper writes task, then starts writer as a thread on the pointer it reads out of it, which
// points at task itself: what the field holds is not proven, and writer keeps task for lack of
// proof. main takes gate then task's lock while the thread runs. A kept object names the locks the
// thread holds at its calls, as it names those the thread takes: the order its call to takeGate
// takes, task's lock then gate, closes the cycle it closes if the thread holds task (#242).
// Expected: one deadlock, between writer's call to takeGate under task's lock and main taking
// gate then task's lock.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    struct Task* self;
    pthread_t thread;
    int value;
};

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static struct Task task = {PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0};

static void takeGate(void)
{
    pthread_mutex_lock(&gate);
    pthread_mutex_unlock(&gate);
}

static void* writer(void* argument)
{
    struct Task* held = argument;
    pthread_mutex_lock(&held->lock);
    takeGate();
    pthread_mutex_unlock(&held->lock);
    return NULL;
}

static void start(struct Task* started)
{
    started->value = 0;
    pthread_create(&started->thread, NULL, writer, started->self);
}

int main(void)
{
    task.self = &task;
    start(&task);
    pthread_mutex_lock(&gate);
    pthread_mutex_lock(&task.lock);
    pthread_mutex_unlock(&task.lock);
    pthread_mutex_unlock(&gate);
    pthread_join(task.thread, NULL);
    return 0;
}
