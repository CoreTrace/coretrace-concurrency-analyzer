// SPDX-License-Identifier: Apache-2.0
// writer increments shared under the lock of the task it is handed. main hands writer and task to
// a helper, which starts writer as a thread on task; main starts reader, which reads shared under
// task's lock, and calls writer on other while both run. The thread and reader hold task's lock;
// main's call holds other's, which excludes neither of them. The spawn binds writer to task, which
// only its thread holds: the lock main's call holds is the one it passes (#235).
// Expected: two data races on shared, main's call against the thread and against reader.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

static struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};
static struct Task other = {PTHREAD_MUTEX_INITIALIZER, 0};
static int shared;

static void* writer(void* argument)
{
    struct Task* held = argument;
    pthread_mutex_lock(&held->lock);
    shared += 1;
    pthread_mutex_unlock(&held->lock);
    return NULL;
}

static void* reader(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&task.lock);
    int value = shared;
    pthread_mutex_unlock(&task.lock);
    return (void*)(long)value;
}

static void start(struct Task* started, void* (*entry)(void*))
{
    pthread_create(&started->thread, NULL, entry, started);
}

int main(void)
{
    pthread_t back;
    start(&task, writer);
    pthread_create(&back, NULL, reader, NULL);
    writer(&other);
    pthread_join(back, NULL);
    pthread_join(task.thread, NULL);
    return shared;
}
