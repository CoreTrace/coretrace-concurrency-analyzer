// SPDX-License-Identifier: Apache-2.0
// One lock lives in a local object handed to the worker, the other in a global array. Each side
// compares the global lock's address with a pointer that is not the local lock's object: the
// worker's key is below it, main's is not, so they take the two locks in opposite orders.
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

struct Context
{
    pthread_mutex_t lock;
    const pthread_mutex_t* below;
    const pthread_mutex_t* same;
};

static pthread_mutex_t locks[2] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};

static void* worker(void* argument)
{
    struct Context* context = argument;
    if (context->below < &locks[1])
    {
        pthread_mutex_lock(&context->lock);
        pthread_mutex_lock(&locks[1]);
    }
    else
    {
        pthread_mutex_lock(&locks[1]);
        pthread_mutex_lock(&context->lock);
    }
    pthread_mutex_unlock(&locks[1]);
    pthread_mutex_unlock(&context->lock);
    return NULL;
}

int main(void)
{
    struct Context context = {PTHREAD_MUTEX_INITIALIZER, &locks[0], &locks[1]};
    pthread_t thread;
    pthread_create(&thread, NULL, worker, &context);
    if (context.same < &locks[1])
    {
        pthread_mutex_lock(&context.lock);
        pthread_mutex_lock(&locks[1]);
    }
    else
    {
        pthread_mutex_lock(&locks[1]);
        pthread_mutex_lock(&context.lock);
    }
    pthread_mutex_unlock(&locks[1]);
    pthread_mutex_unlock(&context.lock);
    pthread_join(thread, NULL);
    return 0;
}
