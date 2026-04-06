// SPDX-License-Identifier: Apache-2.0
// Test M1: one global races while a second one stays protected
#include <pthread.h>

int racy_counter = 0;
int safe_counter = 0;
pthread_mutex_t safe_lock = PTHREAD_MUTEX_INITIALIZER;

void* worker(void* arg)
{
    (void)arg;

    for (int i = 0; i < 1000; ++i)
    {
        racy_counter++;

        pthread_mutex_lock(&safe_lock);
        safe_counter++;
        pthread_mutex_unlock(&safe_lock);
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
