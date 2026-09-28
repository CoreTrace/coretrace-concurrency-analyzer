// SPDX-License-Identifier: Apache-2.0
// The owner takes a mutex of its own, not the object's, around its write: the two writes are not
// under a common lock, and the race stays reported.
#include <pthread.h>
#include <stddef.h>

struct Shared
{
    pthread_mutex_t lock;
    int value;
    int other;
};

static pthread_mutex_t owner_lock = PTHREAD_MUTEX_INITIALIZER;

static void* worker(void* argument)
{
    struct Shared* data = argument;
    pthread_mutex_lock(&data->lock);
    data->value += 1;
    pthread_mutex_unlock(&data->lock);
    return NULL;
}

int main(void)
{
    struct Shared shared = {PTHREAD_MUTEX_INITIALIZER, 0, 0};
    pthread_t thread;
    pthread_create(&thread, NULL, worker, &shared);
    pthread_mutex_lock(&owner_lock);
    shared.value += 1;
    pthread_mutex_unlock(&owner_lock);
    pthread_join(thread, NULL);
    return shared.value;
}
