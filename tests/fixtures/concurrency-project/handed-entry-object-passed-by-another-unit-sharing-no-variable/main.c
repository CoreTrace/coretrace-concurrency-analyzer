// SPDX-License-Identifier: Apache-2.0
// Unit of the entry: writer increments shared under the lock of the task it is handed, and
// startWriter hands writer, with the task it is passed, to a helper starting it as a thread. main
// starts writer on its own task, starts reader on task, which reads shared under task's lock,
// and once the thread is joined calls start_other, which another unit defines. That unit shares
// no variable with this one: only its call to startWriter tells this unit's analysis that writer
// may start on another object (#235).
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

int shared;

void start_other(void);
void join_other(void);

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
    struct Task* held = argument;
    pthread_mutex_lock(&held->lock);
    int value = shared;
    pthread_mutex_unlock(&held->lock);
    return (void*)(long)value;
}

static void start(struct Task* started, void* (*entry)(void*))
{
    pthread_create(&started->thread, NULL, entry, started);
}

void startWriter(struct Task* started)
{
    start(started, writer);
}

int main(void)
{
    struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};
    pthread_t back;
    start(&task, writer);
    pthread_create(&back, NULL, reader, &task);
    pthread_join(task.thread, NULL);
    start_other();
    pthread_join(back, NULL);
    join_other();
    return shared;
}
