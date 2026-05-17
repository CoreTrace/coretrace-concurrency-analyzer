// SPDX-License-Identifier: Apache-2.0
// Test: helper access without a callsite lock should still report a race.
#include <pthread.h>

int shared_counter = 0;

void increment_shared(void)
{
    shared_counter++;
}

void* worker(void* arg)
{
    (void)arg;
    increment_shared();
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
