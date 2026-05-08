// SPDX-License-Identifier: Apache-2.0
// Test: callsite lock protection propagates through nested direct calls.
#include <pthread.h>

pthread_mutex_t shared_lock = PTHREAD_MUTEX_INITIALIZER;
int shared_counter = 0;

void increment_shared(void)
{
    shared_counter++;
}

void outer(void)
{
    increment_shared();
}

void* worker(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&shared_lock);
    outer();
    pthread_mutex_unlock(&shared_lock);
    return 0;
}

int main(void)
{
    pthread_t first;
    pthread_t second;

    pthread_create(&first, 0, worker, 0);
    pthread_create(&second, 0, worker, 0);

    pthread_join(first, 0);
    pthread_join(second, 0);
    return 0;
}
