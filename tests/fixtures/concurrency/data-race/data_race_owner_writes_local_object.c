// SPDX-License-Identifier: Apache-2.0
// The owner hands a local object to a thread and keeps writing the field the thread writes. Both
// reach the same bytes, the owner by name and the thread through its argument.
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
    pthread_t thread;
    pthread_create(&thread, NULL, worker, &shared);
    shared.value += 1;
    pthread_join(thread, NULL);
    return shared.value;
}
