// SPDX-License-Identifier: Apache-2.0
// Test M1: shared global protected by a recognized mutex on every access
#include <pthread.h>

int shared_counter = 0;
pthread_mutex_t shared_lock = PTHREAD_MUTEX_INITIALIZER;

void* worker(void* arg)
{
    (void)arg;

    for (int i = 0; i < 1000; ++i)
    {
        pthread_mutex_lock(&shared_lock);
        shared_counter++;
        pthread_mutex_unlock(&shared_lock);
    }

    return 0;
}

int main()
{
    pthread_t first;
    pthread_t second;

    pthread_create(&first, 0, worker, 0);
    pthread_create(&second, 0, worker, 0);

    pthread_join(first, 0);
    pthread_join(second, 0);
    return 0;
}

// EXPECT-HUMAN-DIAGNOSTICS-BEGIN
// EXPECT-HUMAN-DIAGNOSTICS-END
