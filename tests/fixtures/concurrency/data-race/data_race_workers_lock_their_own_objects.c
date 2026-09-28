// SPDX-License-Identifier: Apache-2.0
// Two threads run the same entry on different objects, each locking the mutex of its own object
// before writing a global. The entry names that mutex after its parameter, which stands for a
// different object in each thread: it is no lock the two threads share, and the race stays.
#include <pthread.h>
#include <stddef.h>

struct Guard
{
    pthread_mutex_t lock;
};

static int total;

static void* worker(void* argument)
{
    struct Guard* guard = argument;
    pthread_mutex_lock(&guard->lock);
    total += 1;
    pthread_mutex_unlock(&guard->lock);
    return NULL;
}

int main(void)
{
    struct Guard first = {PTHREAD_MUTEX_INITIALIZER};
    struct Guard second = {PTHREAD_MUTEX_INITIALIZER};
    pthread_t threads[2];
    pthread_create(&threads[0], NULL, worker, &first);
    pthread_create(&threads[1], NULL, worker, &second);
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);
    return total;
}
