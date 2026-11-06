// SPDX-License-Identifier: Apache-2.0
// worker takes the eleven locks of the task it is handed in order, updates its value, and gate
// inside them. main starts worker as a thread on task and joins it, then starts reverse, which
// takes gate and then the first lock of task, and calls worker on task itself. Under that call
// worker takes task's first lock before gate. Its orders outnumber those a function keeps open
// through a recursion, but no recursion runs here: every one of them reaches the call (#235, #242).
// Expected: one deadlock between gate and task, at reverse and worker.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock[11];
    int value;
};

static struct Task task = {
    {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
     PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
     PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
     PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER},
    0};
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;

static void* worker(void* argument)
{
    struct Task* held = argument;
    pthread_mutex_lock(&held->lock[0]);
    pthread_mutex_lock(&held->lock[1]);
    pthread_mutex_lock(&held->lock[2]);
    pthread_mutex_lock(&held->lock[3]);
    pthread_mutex_lock(&held->lock[4]);
    pthread_mutex_lock(&held->lock[5]);
    pthread_mutex_lock(&held->lock[6]);
    pthread_mutex_lock(&held->lock[7]);
    pthread_mutex_lock(&held->lock[8]);
    pthread_mutex_lock(&held->lock[9]);
    pthread_mutex_lock(&held->lock[10]);
    held->value += 1;
    pthread_mutex_lock(&gate);
    pthread_mutex_unlock(&gate);
    pthread_mutex_unlock(&held->lock[10]);
    pthread_mutex_unlock(&held->lock[9]);
    pthread_mutex_unlock(&held->lock[8]);
    pthread_mutex_unlock(&held->lock[7]);
    pthread_mutex_unlock(&held->lock[6]);
    pthread_mutex_unlock(&held->lock[5]);
    pthread_mutex_unlock(&held->lock[4]);
    pthread_mutex_unlock(&held->lock[3]);
    pthread_mutex_unlock(&held->lock[2]);
    pthread_mutex_unlock(&held->lock[1]);
    pthread_mutex_unlock(&held->lock[0]);
    return NULL;
}

static void* reverse(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&gate);
    pthread_mutex_lock(&task.lock[0]);
    pthread_mutex_unlock(&task.lock[0]);
    pthread_mutex_unlock(&gate);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_t reverser;
    pthread_create(&thread, NULL, worker, &task);
    pthread_join(thread, NULL);
    pthread_create(&reverser, NULL, reverse, NULL);
    worker(&task);
    pthread_join(reverser, NULL);
    return 0;
}
