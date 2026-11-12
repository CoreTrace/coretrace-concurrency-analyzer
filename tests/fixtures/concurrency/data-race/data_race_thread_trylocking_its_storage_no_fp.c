// SPDX-License-Identifier: Apache-2.0
// writer tries the lock of the object it is handed, which also holds its handle: a trylock changes
// only the lock's bytes, so main's join waits for writer (#201).
// Expected: no data race.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

static struct Task task = {PTHREAD_MUTEX_INITIALIZER, 0};
static int shared;

static void* writer(void* argument)
{
    struct Task* held = argument;
    if (pthread_mutex_trylock(&held->lock) == 0)
    {
        shared += 1;
        pthread_mutex_unlock(&held->lock);
    }
    return NULL;
}

int main(void)
{
    pthread_create(&task.thread, NULL, writer, &task);
    pthread_join(task.thread, NULL);
    return shared;
}
