// Synchronization unit: it owns the helpers and holds no lock ordering of its own.
#include <pthread.h>

void take(pthread_mutex_t* lock)
{
    pthread_mutex_lock(lock);
}

void give(pthread_mutex_t* lock)
{
    pthread_mutex_unlock(lock);
}
