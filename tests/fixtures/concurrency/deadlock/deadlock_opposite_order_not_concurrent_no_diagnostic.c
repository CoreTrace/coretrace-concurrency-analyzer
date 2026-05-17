// SPDX-License-Identifier: Apache-2.0
// Test: opposite lock order outside concurrent thread entries should not report deadlock.
#include <pthread.h>

pthread_mutex_t lock_a = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lock_b = PTHREAD_MUTEX_INITIALIZER;

void order_a_then_b(void)
{
    pthread_mutex_lock(&lock_a);
    pthread_mutex_lock(&lock_b);
    pthread_mutex_unlock(&lock_b);
    pthread_mutex_unlock(&lock_a);
}

void order_b_then_a(void)
{
    pthread_mutex_lock(&lock_b);
    pthread_mutex_lock(&lock_a);
    pthread_mutex_unlock(&lock_a);
    pthread_mutex_unlock(&lock_b);
}

int main(void)
{
    order_a_then_b();
    order_b_then_a();
    return 0;
}
