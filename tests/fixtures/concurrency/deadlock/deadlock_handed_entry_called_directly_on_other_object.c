// SPDX-License-Identifier: Apache-2.0
// writer takes the lock of the task it is handed, then gate. main hands writer and its task to a
// helper, which starts writer as a thread on the task; main starts reverser, which takes gate then
// other's lock, and calls writer on other while both run. The spawn binds writer to task, which
// only its thread holds: main's call takes writer's orders on what it passes, other's lock then
// gate, the inverse of reverser's (#235).
// Expected: one deadlock, between main's call taking other's lock then gate and reverser taking
// gate then other's lock.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static struct Task other = {PTHREAD_MUTEX_INITIALIZER, 0};

static void* writer(void* argument)
{
    struct Task* task = argument;
    pthread_mutex_lock(&task->lock);
    pthread_mutex_lock(&gate);
    pthread_mutex_unlock(&gate);
    pthread_mutex_unlock(&task->lock);
    return NULL;
}

static void* reverser(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&gate);
    pthread_mutex_lock(&other.lock);
    pthread_mutex_unlock(&other.lock);
    pthread_mutex_unlock(&gate);
    return NULL;
}

static void start(struct Task* task, void* (*entry)(void*))
{
    pthread_create(&task->thread, NULL, entry, task);
}

int main(void)
{
    struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};
    pthread_t second;
    start(&task, writer);
    pthread_create(&second, NULL, reverser, NULL);
    writer(&other);
    pthread_join(second, NULL);
    pthread_join(task.thread, NULL);
    return 0;
}
