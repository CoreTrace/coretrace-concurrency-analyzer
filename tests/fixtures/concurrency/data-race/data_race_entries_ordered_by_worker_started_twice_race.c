// SPDX-License-Identifier: Apache-2.0
// A worker orders first before second, and main starts two workers at once: the first of one
// runs beside the second of the other. first takes a lock, so they are the only race (#224).
// Expected: one data race, first against second.
#include <pthread.h>
#include <stddef.h>

static int shared;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void* first(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&lock);
    shared += 1;
    pthread_mutex_unlock(&lock);
    return NULL;
}

static void* second(void* argument)
{
    (void)argument;
    return (void*)(long)shared;
}

static void phases(void)
{
    pthread_t a;
    pthread_t b;
    pthread_create(&a, NULL, first, NULL);
    pthread_join(a, NULL);
    pthread_create(&b, NULL, second, NULL);
    pthread_join(b, NULL);
}

static void* worker(void* argument)
{
    (void)argument;
    phases();
    return NULL;
}

int main(void)
{
    pthread_t one;
    pthread_t two;
    pthread_create(&one, NULL, worker, NULL);
    pthread_create(&two, NULL, worker, NULL);
    pthread_join(one, NULL);
    pthread_join(two, NULL);
    return shared;
}
