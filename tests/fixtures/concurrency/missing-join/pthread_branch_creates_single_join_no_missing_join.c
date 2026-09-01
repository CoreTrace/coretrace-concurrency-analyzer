// SPDX-License-Identifier: Apache-2.0
// Regression: exactly one thread is created, on one of two exclusive branches, and the join
// that follows covers both. Counting creation sites reported a leak that cannot happen.
#include <pthread.h>

void* first_worker(void* arg)
{
    return arg;
}

void* second_worker(void* arg)
{
    return arg;
}

int main(int argc, char** argv)
{
    pthread_t handle;
    (void)argv;

    if (argc > 1)
        pthread_create(&handle, 0, first_worker, 0);
    else
        pthread_create(&handle, 0, second_worker, 0);

    pthread_join(handle, 0);
    return 0;
}
