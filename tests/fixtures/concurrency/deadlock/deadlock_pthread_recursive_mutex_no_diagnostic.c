// SPDX-License-Identifier: Apache-2.0
// Regression: a mutex initialized with a non-default type attribute may be reacquired by its
// owner. The attribute was never inspected, so the legal relock looked like a self-deadlock.
#include <pthread.h>

pthread_mutex_t recursive_lock;

void* worker(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&recursive_lock);
    pthread_mutex_lock(&recursive_lock);
    pthread_mutex_unlock(&recursive_lock);
    pthread_mutex_unlock(&recursive_lock);
    return 0;
}

int main(void)
{
    pthread_mutexattr_t attributes;
    pthread_t handle;

    pthread_mutexattr_init(&attributes);
    pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&recursive_lock, &attributes);

    pthread_create(&handle, 0, worker, 0);
    pthread_join(handle, 0);
    pthread_mutexattr_destroy(&attributes);
    return 0;
}
