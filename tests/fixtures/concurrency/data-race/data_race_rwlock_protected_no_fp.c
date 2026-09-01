// SPDX-License-Identifier: Apache-2.0
// Regression: a reader/writer lock protects the shared value. The rwlock API was not
// recognized, and the lock object itself was reported as racing data.
#include <pthread.h>

pthread_rwlock_t guard = PTHREAD_RWLOCK_INITIALIZER;
int guarded_value;

void* writer(void* arg)
{
    (void)arg;
    pthread_rwlock_wrlock(&guard);
    guarded_value++;
    pthread_rwlock_unlock(&guard);
    return 0;
}

void* reader(void* arg)
{
    (void)arg;
    pthread_rwlock_rdlock(&guard);
    (void)guarded_value;
    pthread_rwlock_unlock(&guard);
    return 0;
}

int main(void)
{
    pthread_t first, second;
    pthread_create(&first, 0, writer, 0);
    pthread_create(&second, 0, reader, 0);
    pthread_join(first, 0);
    pthread_join(second, 0);
    return guarded_value;
}
