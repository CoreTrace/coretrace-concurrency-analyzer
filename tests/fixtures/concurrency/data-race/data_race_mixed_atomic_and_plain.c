// SPDX-License-Identifier: Apache-2.0
// Regression: an atomic read-modify-write on one side and a plain increment on the other
// still race. Atomic RMW instructions produced no access fact at all, so the conflict had
// only one side and was never reported.
#include <pthread.h>

int mixed_counter;

void* atomic_worker(void* arg)
{
    (void)arg;
    __sync_fetch_and_add(&mixed_counter, 1);
    return 0;
}

void* plain_worker(void* arg)
{
    (void)arg;
    mixed_counter++;
    return 0;
}

int main(void)
{
    pthread_t first, second;
    pthread_create(&first, 0, atomic_worker, 0);
    pthread_create(&second, 0, plain_worker, 0);
    pthread_join(first, 0);
    pthread_join(second, 0);
    return mixed_counter;
}
