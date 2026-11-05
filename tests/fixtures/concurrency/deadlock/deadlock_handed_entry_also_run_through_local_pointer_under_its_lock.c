// SPDX-License-Identifier: Apache-2.0
// writer takes first then second under the lock of the task it is handed. main hands writer and
// task to a helper, which starts writer as a thread on task, and starts reverser, which takes
// second then first under task's lock: the thread and reverser exclude each other. main also runs
// writer on other through a local pointer, under other's lock, which excludes nothing reverser
// holds. A call through a copy of writer's address runs it where no spawn names its object (#235).
// Expected: one deadlock on first and second, between writer and reverser.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

static struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};
static struct Task other = {PTHREAD_MUTEX_INITIALIZER, 0};
static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;

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
    void* (*run)(void*) = writer;
    run(&other);
    pthread_join(back, NULL);
    pthread_join(task.thread, NULL);
    return 0;
}
