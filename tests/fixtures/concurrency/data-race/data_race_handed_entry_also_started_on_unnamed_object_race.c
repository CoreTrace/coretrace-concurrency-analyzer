// SPDX-License-Identifier: Apache-2.0
// writer increments shared under the lock of the task it is handed. main hands writer to a helper
// that starts it as a thread, once on task and once on a task newly allocated, which only the
// call's result designates, and starts reader, which reads shared under task's lock. The second
// thread holds its own task's lock, which excludes neither the first thread nor reader. No object
// names what that call passes, so no spawn binds writer to task (#235).
// Expected: two data races on shared, between the two runs of writer and between reader and writer.
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

static struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};
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

static struct Task* made;

static struct Task* makeTask(void)
{
    made = malloc(sizeof *made);
    pthread_mutex_init(&made->lock, NULL);
    return made;
}

int main(void)
{
    pthread_t back;
    start(&task, writer);
    start(makeTask(), writer);
    pthread_create(&back, NULL, reader, NULL);
    pthread_join(back, NULL);
    pthread_join(made->thread, NULL);
    pthread_join(task.thread, NULL);
    return 0;
}
