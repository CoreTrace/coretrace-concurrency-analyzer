// SPDX-License-Identifier: Apache-2.0
// writer takes the lock of the task it is handed, then gate. A helper starts it as a thread on
// main's task; main starts reverser, which takes other's lock then gate, and calls writer on other
// while both run. main's call takes writer's orders on what it passes, other's lock then gate, in
// reverser's order, and the thread takes task's lock then gate: no two orders are inverse.
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static struct Task other = {PTHREAD_MUTEX_INITIALIZER, 0};

static void* writer(void* argument)
{
    struct Task* task = argument;
    pthread_mutex_lock(&task->lock);
    pthread_mutex_lock(&gate);
    pthread_mutex_unlock(&gate);
    pthread_mutex_unlock(&task->lock);
    return NULL;
}

static void* reverser(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&other.lock);
    pthread_mutex_lock(&gate);
    pthread_mutex_unlock(&gate);
    pthread_mutex_unlock(&other.lock);
    return NULL;
}

static void start(struct Task* task)
{
    pthread_create(&task->thread, NULL, writer, task);
}

int main(void)
{
    struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};
    pthread_t second;
    start(&task);
    pthread_create(&second, NULL, reverser, NULL);
    writer(&other);
    pthread_join(second, NULL);
    pthread_join(task.thread, NULL);
    return 0;
}
