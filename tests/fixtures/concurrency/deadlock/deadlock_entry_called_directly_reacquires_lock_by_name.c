// SPDX-License-Identifier: Apache-2.0
// writer takes the lock of the task it is handed, then gate twice, and hands spare to relock,
// which takes it twice. main starts writer as a thread on task and calls it on other while it
// runs. Every run of writer reacquires gate and spare, which it names itself: each reacquisition
// is reported once, where writer takes it or hands it on, whichever runs take it (#235).
// Expected: two deadlocks, writer reacquiring gate, and spare at its call to relock.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    int value;
};

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t spare = PTHREAD_MUTEX_INITIALIZER;
static struct Task other = {PTHREAD_MUTEX_INITIALIZER, 0};

static void relock(pthread_mutex_t* lock)
{
    pthread_mutex_lock(lock);
    pthread_mutex_lock(lock);
    pthread_mutex_unlock(lock);
    pthread_mutex_unlock(lock);
}

static void* writer(void* argument)
{
    struct Task* task = argument;
    pthread_mutex_lock(&task->lock);
    pthread_mutex_lock(&gate);
    pthread_mutex_lock(&gate);
    pthread_mutex_unlock(&gate);
    pthread_mutex_unlock(&gate);
    relock(&spare);
    pthread_mutex_unlock(&task->lock);
    return NULL;
}

int main(void)
{
    struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};
    pthread_t thread;
    pthread_create(&thread, NULL, writer, &task);
    writer(&other);
    pthread_join(thread, NULL);
    return 0;
}
