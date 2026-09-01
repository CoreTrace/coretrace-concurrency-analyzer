// SPDX-License-Identifier: Apache-2.0
// Regression: the initial thread writes between the spawn and the join. `main` was not
// modelled as a task, so the most common data race of all went unreported.
#include <pthread.h>

int escaping_counter;

void* worker(void* arg)
{
    (void)arg;
    escaping_counter++;
    return 0;
}

int main(void)
{
    pthread_t handle;
    pthread_create(&handle, 0, worker, 0);
    escaping_counter++;
    pthread_join(handle, 0);
    return escaping_counter;
}
