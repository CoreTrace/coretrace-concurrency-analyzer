// SPDX-License-Identifier: Apache-2.0
// Regression: both workers take the same outer lock before the inner pair, so the opposite
// inner orders can never interleave. The common gate was not considered.
#include <pthread.h>

pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t first_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t second_lock = PTHREAD_MUTEX_INITIALIZER;

void* forward_order(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&gate);
    pthread_mutex_lock(&first_lock);
    pthread_mutex_lock(&second_lock);
    pthread_mutex_unlock(&second_lock);
    pthread_mutex_unlock(&first_lock);
    pthread_mutex_unlock(&gate);
    return 0;
}

void* reverse_order(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&gate);
    pthread_mutex_lock(&second_lock);
    pthread_mutex_lock(&first_lock);
    pthread_mutex_unlock(&first_lock);
    pthread_mutex_unlock(&second_lock);
    pthread_mutex_unlock(&gate);
    return 0;
}

int main(void)
{
    pthread_t first, second;
    pthread_create(&first, 0, forward_order, 0);
    pthread_create(&second, 0, reverse_order, 0);
    pthread_join(first, 0);
    pthread_join(second, 0);
    return 0;
}
