// SPDX-License-Identifier: Apache-2.0
// Regression: both branches spawn and join the same entry, so a single instance ever runs.
// Counting spawn sites made the entry look self-concurrent.
#include <pthread.h>

int branch_counter;

void* worker(void* arg)
{
    (void)arg;
    branch_counter = 7;
    return 0;
}

int main(int argc, char** argv)
{
    pthread_t handle;
    (void)argv;

    if (argc > 1)
    {
        pthread_create(&handle, 0, worker, 0);
        pthread_join(handle, 0);
    }
    else
    {
        pthread_create(&handle, 0, worker, 0);
        pthread_join(handle, 0);
    }

    return branch_counter;
}
