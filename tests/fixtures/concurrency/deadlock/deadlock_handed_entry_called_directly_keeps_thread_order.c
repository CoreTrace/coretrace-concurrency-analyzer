// SPDX-License-Identifier: Apache-2.0
// writer takes the lock of the task it is handed, then gate. main hands writer and task to a
// helper, which starts writer as a thread on task; main starts reverser, which takes gate then
// task's lock, and calls writer on other while both run. The thread takes task's lock then gate,
// the inverse of reverser's order; main's call takes other's lock then gate, which closes nothing.
// The thread's orders are named after the task its spawn hands it, apart from the call's (#235).
// Expected: one deadlock, between the thread taking task's lock then gate and reverser taking gate
// then task's lock.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};
static struct Task other = {PTHREAD_MUTEX_INITIALIZER, 0};

static void* writer(void* argument)
{
    struct Task* held = argument;
    pthread_mutex_lock(&held->lock);
    pthread_mutex_lock(&gate);
    pthread_mutex_unlock(&gate);
    pthread_mutex_unlock(&held->lock);
    return NULL;
}

static void* reverser(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&gate);
    pthread_mutex_lock(&task.lock);
    pthread_mutex_unlock(&task.lock);
    pthread_mutex_unlock(&gate);
    return NULL;
}

static void start(struct Task* started, void* (*entry)(void*))
{
    pthread_create(&started->thread, NULL, entry, started);
}

int main(void)
{
    pthread_t back;
    start(&task, writer);
    pthread_create(&back, NULL, reverser, NULL);
    writer(&other);
    pthread_join(back, NULL);
    pthread_join(task.thread, NULL);
    return 0;
}
