// SPDX-License-Identifier: Apache-2.0
// worker takes the eleven locks of the task it is handed, the first one last, updates its value,
// and calls itself once more on the same task while it holds outer, a recursive mutex. main starts
// worker as a thread on task and joins it, then starts reverse, which takes the first lock of task
// and then outer, and calls worker on task itself. Under that call, the recursion takes task's
// first lock while outer is held. The recursion names no lock worker does not name already: every
// order it brings is kept, the one on its last lock among them (#235, #242).
// Expected: one deadlock between outer and task, at reverse and at main's call of worker.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock[11];
    int value;
    int depth;
};

static struct Task task = {
    {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
     PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
     PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
     PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER},
    0,
    1};
static pthread_mutex_t outer;

static void* worker(void* argument)
{
    struct Task* held = argument;
    pthread_mutex_lock(&held->lock[10]);
    pthread_mutex_lock(&held->lock[9]);
    pthread_mutex_lock(&held->lock[8]);
    pthread_mutex_lock(&held->lock[7]);
    pthread_mutex_lock(&held->lock[6]);
    pthread_mutex_lock(&held->lock[5]);
    pthread_mutex_lock(&held->lock[4]);
    pthread_mutex_lock(&held->lock[3]);
    pthread_mutex_lock(&held->lock[2]);
    pthread_mutex_lock(&held->lock[1]);
    pthread_mutex_lock(&held->lock[0]);
    held->value += 1;
    pthread_mutex_unlock(&held->lock[0]);
    pthread_mutex_unlock(&held->lock[1]);
    pthread_mutex_unlock(&held->lock[2]);
    pthread_mutex_unlock(&held->lock[3]);
    pthread_mutex_unlock(&held->lock[4]);
    pthread_mutex_unlock(&held->lock[5]);
    pthread_mutex_unlock(&held->lock[6]);
    pthread_mutex_unlock(&held->lock[7]);
    pthread_mutex_unlock(&held->lock[8]);
    pthread_mutex_unlock(&held->lock[9]);
    pthread_mutex_unlock(&held->lock[10]);
    if (held->depth > 0)
    {
        held->depth -= 1;
        pthread_mutex_lock(&outer);
        worker(held);
        pthread_mutex_unlock(&outer);
    }
    return NULL;
}

static void* reverse(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&task.lock[0]);
    pthread_mutex_lock(&outer);
    pthread_mutex_unlock(&outer);
    pthread_mutex_unlock(&task.lock[0]);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_t reverser;
    pthread_mutexattr_t attributes;
    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&outer, &attributes);
    pthread_create(&thread, NULL, worker, &task);
    pthread_join(thread, NULL);
    pthread_create(&reverser, NULL, reverse, NULL);
    worker(&task);
    pthread_join(reverser, NULL);
    pthread_mutexattr_destroy(&attributes);
    return 0;
}
