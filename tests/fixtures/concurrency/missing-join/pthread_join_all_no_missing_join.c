// SPDX-License-Identifier: Apache-2.0
// Test: all pthread handles are joined and should not produce MissingJoin.
#include <pthread.h>

void* worker(void* arg)
{
    (void)arg;
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
