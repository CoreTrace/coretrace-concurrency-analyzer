// SPDX-License-Identifier: Apache-2.0
// writer increments shared under the lock of the task it is handed. main hands writer and task to
// a helper, which starts writer as a thread on task, and starts reader, which reads shared under
// task's lock. Once the thread is joined, main hands the same helper other and a copy holding
// writer or idle, whichever argc picks: the thread may run writer on other, under other's lock,
// while reader runs. A call no spawn settles for writer may still start it (#235).
// Expected: one data race on shared, between reader and writer.
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

static void* idle(void* argument)
{
    return argument;
}

int main(int argc, char** argv)
{
    (void)argv;
    pthread_t back;
    start(&task, writer);
    pthread_create(&back, NULL, reader, NULL);
    pthread_join(task.thread, NULL);
    void* (*chosen)(void*) = writer;
    if (argc > 1)
        chosen = idle;
    start(&other, chosen);
    pthread_join(other.thread, NULL);
    pthread_join(back, NULL);
    return 0;
}
