// SPDX-License-Identifier: Apache-2.0
// Regression: the initial thread takes the locks in one order while a spawned worker takes
// them in the other. Lock orders reached only from `main` were excluded from the search.
#include <pthread.h>

pthread_mutex_t first_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t second_lock = PTHREAD_MUTEX_INITIALIZER;

void* reverse_order(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&second_lock);
    pthread_mutex_lock(&first_lock);
    pthread_mutex_unlock(&first_lock);
    pthread_mutex_unlock(&second_lock);
    return 0;
}

int main(void)
{
    pthread_t handle;
    pthread_create(&handle, 0, reverse_order, 0);

    pthread_mutex_lock(&first_lock);
    pthread_mutex_lock(&second_lock);
    pthread_mutex_unlock(&second_lock);
    pthread_mutex_unlock(&first_lock);

    pthread_join(handle, 0);
    return 0;
}
