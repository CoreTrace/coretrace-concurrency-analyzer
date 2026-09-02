// SPDX-License-Identifier: Apache-2.0
// A thread-safe object keeps its mutex beside the data it guards. Once the object is identified
// so is the lock, at the same base and its own offset — otherwise the data would be nameable and
// the lock protecting it would not, and every correctly guarded access would read as a race.
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>

struct Guarded
{
    pthread_mutex_t lock;
    int value;
};

static void* increment(void* argument)
{
    struct Guarded* guarded = (struct Guarded*)argument;

    pthread_mutex_lock(&guarded->lock);
    guarded->value = guarded->value + 1;
    pthread_mutex_unlock(&guarded->lock);
    return NULL;
}

int main(void)
{
    struct Guarded* shared = malloc(sizeof *shared);
    pthread_t first;
    pthread_t second;

    pthread_mutex_init(&shared->lock, NULL);
    pthread_create(&first, NULL, increment, shared);
    pthread_create(&second, NULL, increment, shared);
    pthread_join(first, NULL);
    pthread_join(second, NULL);

    free(shared);
    return 0;
}
