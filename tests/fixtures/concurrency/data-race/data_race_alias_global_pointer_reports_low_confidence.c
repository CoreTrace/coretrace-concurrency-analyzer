// SPDX-License-Identifier: Apache-2.0
// Test: opaque pointer return may alias a tracked global and should lower confidence.
#include <pthread.h>

int shared_counter = 0;

extern int* get_shared_counter_alias(void);

void* worker(void* arg)
{
    (void)arg;
    int* target = get_shared_counter_alias();
    *target += 1;
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
