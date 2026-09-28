// SPDX-License-Identifier: Apache-2.0
// Both sides write the global object's field under the object's own mutex, the owner by name and
// the thread through its argument.
#include <pthread.h>
#include <stddef.h>

struct Shared
{
    pthread_mutex_t lock;
    int value;
    int other;
};

static struct Shared shared = {PTHREAD_MUTEX_INITIALIZER, 0, 0};

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
    pthread_t thread;
    pthread_create(&thread, NULL, worker, &shared);
    pthread_mutex_lock(&shared.lock);
    shared.value += 1;
    pthread_mutex_unlock(&shared.lock);
    pthread_join(thread, NULL);
    return shared.value;
}
