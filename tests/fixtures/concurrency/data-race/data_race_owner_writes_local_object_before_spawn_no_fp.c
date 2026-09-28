// SPDX-License-Identifier: Apache-2.0
// The owner writes its local object before handing it to the thread: the spawn orders the write
// before everything the thread does.
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
    data->value += 1;
    return NULL;
}

int main(void)
{
    struct Shared shared = {PTHREAD_MUTEX_INITIALIZER, 0, 0};
    shared.value = 1;
    pthread_t thread;
    pthread_create(&thread, NULL, worker, &shared);
    pthread_join(thread, NULL);
    return shared.value;
}
