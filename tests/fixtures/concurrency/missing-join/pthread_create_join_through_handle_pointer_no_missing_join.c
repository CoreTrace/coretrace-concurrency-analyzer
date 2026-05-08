// SPDX-License-Identifier: Apache-2.0
// Test: pthread lifecycle resolution follows a local pointer to the handle.
#include <pthread.h>

void* worker(void* arg)
{
    (void)arg;
    return 0;
}

int main(void)
{
    pthread_t handle;
    pthread_t* handle_ref = &handle;

    pthread_create(handle_ref, 0, worker, 0);
    pthread_join(*handle_ref, 0);
    return 0;
}
