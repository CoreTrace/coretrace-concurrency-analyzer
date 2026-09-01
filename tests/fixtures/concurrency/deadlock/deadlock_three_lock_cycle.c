// SPDX-License-Identifier: Apache-2.0
// Regression: a genuine three-lock wait-for cycle. Only exact two-lock inversions were
// searched, so no cycle longer than a pair was ever found.
#include <pthread.h>

pthread_mutex_t first_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t second_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t third_lock = PTHREAD_MUTEX_INITIALIZER;

void* first_to_second(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&first_lock);
    pthread_mutex_lock(&second_lock);
    pthread_mutex_unlock(&second_lock);
    pthread_mutex_unlock(&first_lock);
    return 0;
}

void* second_to_third(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&second_lock);
    pthread_mutex_lock(&third_lock);
    pthread_mutex_unlock(&third_lock);
    pthread_mutex_unlock(&second_lock);
    return 0;
}

void* third_to_first(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&third_lock);
    pthread_mutex_lock(&first_lock);
    pthread_mutex_unlock(&first_lock);
    pthread_mutex_unlock(&third_lock);
    return 0;
}

int main(void)
{
    pthread_t first, second, third;
    pthread_create(&first, 0, first_to_second, 0);
    pthread_create(&second, 0, second_to_third, 0);
    pthread_create(&third, 0, third_to_first, 0);
    pthread_join(first, 0);
    pthread_join(second, 0);
    pthread_join(third, 0);
    return 0;
}
