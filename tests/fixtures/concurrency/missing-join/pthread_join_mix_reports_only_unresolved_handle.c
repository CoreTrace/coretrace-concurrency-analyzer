// SPDX-License-Identifier: Apache-2.0
// Test: one joined pthread and one unresolved pthread should report one MissingJoin.
#include <pthread.h>

void* worker(void* arg)
{
    (void)arg;
    return 0;
}

int main(void)
{
    pthread_t joined;
    pthread_t unresolved;

    pthread_create(&joined, 0, worker, 0);
    pthread_create(&unresolved, 0, worker, 0);

    pthread_join(joined, 0);
    return 0;
}
