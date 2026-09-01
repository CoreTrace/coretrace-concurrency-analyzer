// SPDX-License-Identifier: Apache-2.0
// Regression: two distinct mutexes stored side by side in one global, acquired in a single
// consistent order. Collapsing the aggregate onto its base symbol made both ids identical
// and produced a bogus "reacquiring a non-recursive lock".
#include <pthread.h>

struct lock_pair
{
    pthread_mutex_t first;
    pthread_mutex_t second;
};

struct lock_pair locks = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};

void* worker(void* arg)
{
    (void)arg;
    pthread_mutex_lock(&locks.first);
    pthread_mutex_lock(&locks.second);
    pthread_mutex_unlock(&locks.second);
    pthread_mutex_unlock(&locks.first);
    return 0;
}

int main(void)
{
    pthread_t handle;
    pthread_create(&handle, 0, worker, 0);
    pthread_join(handle, 0);
    return 0;
}
