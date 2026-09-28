// SPDX-License-Identifier: Apache-2.0
// Both sides update the local object through a helper that takes the object's own mutex. The
// helper names the lock after its parameter; each call site names it after the object passed.
#include <pthread.h>
#include <stddef.h>

struct Shared
{
    pthread_mutex_t lock;
    int value;
    int other;
};

static void bump(struct Shared* target)
{
    pthread_mutex_lock(&target->lock);
    target->value += 1;
    pthread_mutex_unlock(&target->lock);
}

static void* worker(void* argument)
{
    struct Shared* data = argument;
    bump(data);
    return NULL;
}

int main(void)
{
    struct Shared shared = {PTHREAD_MUTEX_INITIALIZER, 0, 0};
    pthread_t thread;
    pthread_create(&thread, NULL, worker, &shared);
    bump(&shared);
    pthread_join(thread, NULL);
    return shared.value;
}
