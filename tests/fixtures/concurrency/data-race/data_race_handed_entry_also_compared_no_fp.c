// SPDX-License-Identifier: Apache-2.0
// writer increments shared under the lock of the task it is handed. main hands writer and task to
// a helper, which starts writer as a thread on task, and hands writer to a helper that only
// compares it; reader reads shared under task's lock. A comparison runs nothing: every run of
// writer is the thread's, bound to task, and holds task's lock as reader does (#235).
// Expected: no data race.
#include <pthread.h>
#include <stddef.h>

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

static void* (*remembered)(void*);

static int isRemembered(void* (*entry)(void*))
{
    return entry == remembered;
}

int main(void)
{
    pthread_t back;
    start(&task, writer);
    (void)isRemembered(writer);
    pthread_create(&back, NULL, reader, NULL);
    pthread_join(back, NULL);
    pthread_join(task.thread, NULL);
    return shared;
}
