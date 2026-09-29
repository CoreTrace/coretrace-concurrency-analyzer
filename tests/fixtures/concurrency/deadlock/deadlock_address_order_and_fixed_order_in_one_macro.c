// SPDX-License-Identifier: Apache-2.0
// One macro takes the two locks lower address first, then again in a fixed order. Every
// acquisition it expands to shares the line of its use. When `second` is the lower, a thread in the
// fixed part holds `first` and waits for `second`, held by one in the address-ordered part.
// Expected: one deadlock.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t first = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second = PTHREAD_MUTEX_INITIALIZER;

#define LOCK_BY_ADDRESS_THEN_IN_ORDER(x, y)                                                       \
    do                                                                                             \
    {                                                                                              \
        if (&(x) < &(y))                                                                           \
        {                                                                                          \
            pthread_mutex_lock(&(x));                                                              \
            pthread_mutex_lock(&(y));                                                              \
        }                                                                                          \
        else                                                                                       \
        {                                                                                          \
            pthread_mutex_lock(&(y));                                                              \
            pthread_mutex_lock(&(x));                                                              \
        }                                                                                          \
        pthread_mutex_unlock(&(y));                                                                \
        pthread_mutex_unlock(&(x));                                                                \
        pthread_mutex_lock(&(x));                                                                  \
        pthread_mutex_lock(&(y));                                                                  \
        pthread_mutex_unlock(&(y));                                                                \
        pthread_mutex_unlock(&(x));                                                                \
    } while (0)

static void* worker(void* argument)
{
    (void)argument;
    LOCK_BY_ADDRESS_THEN_IN_ORDER(first, second);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    LOCK_BY_ADDRESS_THEN_IN_ORDER(first, second);
    pthread_join(thread, NULL);
    return 0;
}
