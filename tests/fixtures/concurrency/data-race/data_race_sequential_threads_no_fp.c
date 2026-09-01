// SPDX-License-Identifier: Apache-2.0
// Regression: the first worker is joined before the second is spawned, so the two never
// overlap. Without happens-before the analyzer treated any two entries as concurrent.
#include <pthread.h>

int sequential_counter;

void* first_worker(void* arg)
{
    (void)arg;
    sequential_counter = 1;
    return 0;
}

void* second_worker(void* arg)
{
    (void)arg;
    sequential_counter = 2;
    return 0;
}

int main(void)
{
    pthread_t first, second;
    pthread_create(&first, 0, first_worker, 0);
    pthread_join(first, 0);
    pthread_create(&second, 0, second_worker, 0);
    pthread_join(second, 0);
    return sequential_counter;
}
