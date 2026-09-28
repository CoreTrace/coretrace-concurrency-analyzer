// SPDX-License-Identifier: Apache-2.0
// Two helpers each lock the mutex of the object they are handed, then write the same global. The
// objects differ, so the mutexes do: a lock named after each helper's parameter must become the
// lock of what its caller passed, never a name the two helpers happen to share.
#include <pthread.h>
#include <stddef.h>

struct Guard
{
    pthread_mutex_t lock;
};

static struct Guard first_guard = {PTHREAD_MUTEX_INITIALIZER};
static struct Guard second_guard = {PTHREAD_MUTEX_INITIALIZER};
static int total;

static void add_under(struct Guard* guard)
{
    pthread_mutex_lock(&guard->lock);
    total += 1;
    pthread_mutex_unlock(&guard->lock);
}

static void subtract_under(struct Guard* guard)
{
    pthread_mutex_lock(&guard->lock);
    total -= 1;
    pthread_mutex_unlock(&guard->lock);
}

static void* worker(void* argument)
{
    (void)argument;
    add_under(&first_guard);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    subtract_under(&second_guard);
    pthread_join(thread, NULL);
    return total;
}
