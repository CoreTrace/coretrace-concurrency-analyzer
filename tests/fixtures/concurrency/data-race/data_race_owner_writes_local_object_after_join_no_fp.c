// SPDX-License-Identifier: Apache-2.0
// The owner writes its local object after joining the thread it handed it to.
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
    pthread_join(thread, NULL);
    shared.value += 1;
    return shared.value;
}
