// SPDX-License-Identifier: Apache-2.0
// writer takes the lock of the task it is handed, then gate. main starts it as a thread on task
// and joins it, then starts reverser, which takes gate then task's lock, and calls writer on other
// while reverser runs. The thread's order, task's lock then gate, is the inverse of reverser's,
// but the thread has ended before reverser starts; main's call takes other's lock then gate,
// which closes nothing. The thread's orders are the thread's alone, not those of main, which
// calls writer (#235).
// Expected: no diagnostic.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    int value;
};

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};
static struct Task other = {PTHREAD_MUTEX_INITIALIZER, 0};

static void* writer(void* argument)
{
    struct Task* held = argument;
    pthread_mutex_lock(&held->lock);
    pthread_mutex_lock(&gate);
    pthread_mutex_unlock(&gate);
    pthread_mutex_unlock(&held->lock);
    return NULL;
}

static void* reverser(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&gate);
    pthread_mutex_lock(&task.lock);
    pthread_mutex_unlock(&task.lock);
    pthread_mutex_unlock(&gate);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_t back;
    pthread_create(&thread, NULL, writer, &task);
    pthread_join(thread, NULL);
    pthread_create(&back, NULL, reverser, NULL);
    writer(&other);
    pthread_join(back, NULL);
    return 0;
}
