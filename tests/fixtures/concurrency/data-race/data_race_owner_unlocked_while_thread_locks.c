// SPDX-License-Identifier: Apache-2.0
// The thread takes the object's mutex, the owner does not: one locked side does not protect the
// other, and the race stays reported.
#include <pthread.h>
#include <stddef.h>

struct Shared
{
    pthread_mutex_t lock;
    int value;
    int other;
};

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
    shared.value += 1;
    pthread_join(thread, NULL);
    return shared.value;
}
