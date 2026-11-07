// SPDX-License-Identifier: Apache-2.0
// writer takes the lock of the task it is handed around first then second. main hands writer and
// task to a helper, which starts writer as a thread on task; main starts reverser, which takes
// task's lock around second then first, and calls writer on task too while both run. Every
// inversion of first and second is taken under task's lock, which serializes it: the thread's,
// which its spawn binds to task, and main's call, which passes task (#235).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;
static struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};

static void* writer(void* argument)
{
    struct Task* held = argument;
    pthread_mutex_lock(&held->lock);
    pthread_mutex_lock(&first);
    pthread_mutex_lock(&second);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&held->lock);
    return NULL;
}

static void* reverser(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&task.lock);
    pthread_mutex_lock(&second);
    pthread_mutex_lock(&first);
    pthread_mutex_unlock(&first);
    pthread_mutex_unlock(&second);
    pthread_mutex_unlock(&task.lock);
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
    writer(&task);
    pthread_join(back, NULL);
    pthread_join(task.thread, NULL);
    return 0;
}
