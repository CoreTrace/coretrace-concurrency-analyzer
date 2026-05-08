// SPDX-License-Identifier: Apache-2.0
// Test: concurrent workers use the same lock order and should not report deadlock.
#include <pthread.h>

pthread_mutex_t lock_a = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lock_b = PTHREAD_MUTEX_INITIALIZER;

void take_locks_in_order(void)
{
    pthread_mutex_lock(&lock_a);
    pthread_mutex_lock(&lock_b);
    pthread_mutex_unlock(&lock_b);
    pthread_mutex_unlock(&lock_a);
}

void* first_worker(void* arg)
{
    (void)arg;
    take_locks_in_order();
    return 0;
}

void* second_worker(void* arg)
{
    (void)arg;
    take_locks_in_order();
    return 0;
}

int main(void)
{
    pthread_t first;
    pthread_t second;

    pthread_create(&first, 0, first_worker, 0);
    pthread_create(&second, 0, second_worker, 0);

    pthread_join(first, 0);
    pthread_join(second, 0);
    return 0;
}
