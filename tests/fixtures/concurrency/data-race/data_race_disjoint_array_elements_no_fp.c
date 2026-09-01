// SPDX-License-Identifier: Apache-2.0
// Regression: each worker owns a distinct element of a global array. Field-insensitive
// resolution used to merge them into one conflict on `partitioned`.
#include <pthread.h>

int partitioned[2];

void* fill_first(void* arg)
{
    (void)arg;
    partitioned[0] = 10;
    return 0;
}

void* fill_second(void* arg)
{
    (void)arg;
    partitioned[1] = 20;
    return 0;
}

int main(void)
{
    pthread_t first, second;
    pthread_create(&first, 0, fill_first, 0);
    pthread_create(&second, 0, fill_second, 0);
    pthread_join(first, 0);
    pthread_join(second, 0);
    return 0;
}
