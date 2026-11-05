// SPDX-License-Identifier: Apache-2.0
// Unit of the entry: writer increments shared under the lock of the task it is handed. main
// hands writer and task to a helper, which starts writer as a thread on task, and starts reader,
// which reads shared under task's lock. Once the thread is joined, main calls run_other, which
// another unit defines, while reader runs. The spawn binds writer to task only when no other
// unit refers to writer, and may run it on what it passes (#235).
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};
struct Task other = {PTHREAD_MUTEX_INITIALIZER, 0};
int shared;

void run_other(void);

void* writer(void* argument)
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
    pthread_join(task.thread, NULL);
    run_other();
    pthread_join(back, NULL);
    return shared;
}
