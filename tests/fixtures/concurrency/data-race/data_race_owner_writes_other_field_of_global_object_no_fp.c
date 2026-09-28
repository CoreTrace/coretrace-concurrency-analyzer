// SPDX-License-Identifier: Apache-2.0
// The owner writes a field of the global object that the thread it handed the object to never
// touches.
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
    data->value += 1;
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, &shared);
    shared.other += 1;
    pthread_join(thread, NULL);
    return shared.value;
}
