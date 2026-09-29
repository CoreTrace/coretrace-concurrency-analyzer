// SPDX-License-Identifier: Apache-2.0
// The two locks belong to one object, and each thread compares pointers to them, which comes out
// the same way in both. The worker then takes the lower one first, main the higher one: their
// orders are opposite. Comparing the object with itself cannot tell the two locks apart.
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

struct Pair
{
    pthread_mutex_t left;
    pthread_mutex_t right;
};

static struct Pair pair = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};
static int shared;

static void* worker(void* argument)
{
    (void)argument;
    pthread_mutex_t* left = &pair.left;
    pthread_mutex_t* right = &pair.right;
    if (left < right)
    {
        pthread_mutex_lock(left);
        pthread_mutex_lock(right);
    }
    else
    {
        pthread_mutex_lock(right);
        pthread_mutex_lock(left);
    }
    ++shared;
    pthread_mutex_unlock(right);
    pthread_mutex_unlock(left);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    pthread_mutex_t* left = &pair.left;
    pthread_mutex_t* right = &pair.right;
    if (left < right)
    {
        pthread_mutex_lock(right);
        pthread_mutex_lock(left);
    }
    else
    {
        pthread_mutex_lock(left);
        pthread_mutex_lock(right);
    }
    ++shared;
    pthread_mutex_unlock(right);
    pthread_mutex_unlock(left);
    pthread_join(thread, NULL);
    return shared;
}
