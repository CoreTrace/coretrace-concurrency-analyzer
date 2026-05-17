// SPDX-License-Identifier: Apache-2.0
// Test: Access helper called under lock should stay protected after callsite projection
#include <pthread.h>

pthread_mutex_t shared_lock = PTHREAD_MUTEX_INITIALIZER;
int shared_counter = 0;

void increment_shared(void)
{
    shared_counter++;
}

void* worker(void* arg)
{
    (void)arg;

    for (int index = 0; index < 1000; ++index)
    {
        pthread_mutex_lock(&shared_lock);
        increment_shared();
        pthread_mutex_unlock(&shared_lock);
    }

    return NULL;
}

int main(void)
{
    pthread_t first;
    pthread_t second;

    pthread_create(&first, NULL, worker, NULL);
    pthread_create(&second, NULL, worker, NULL);

    pthread_join(first, NULL);
    pthread_join(second, NULL);
    return 0;
}
