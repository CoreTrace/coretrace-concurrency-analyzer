// The access is protected, but only through helpers. The summary that closes the deadlock case
// must not stop here: treating `take` as opaque reports a race that cannot happen.
#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static int shared_total = 0;

static void take(pthread_mutex_t* lock)
{
    pthread_mutex_lock(lock);
}

static void give(pthread_mutex_t* lock)
{
    pthread_mutex_unlock(lock);
}

static void* worker(void* argument)
{
    (void)argument;
    take(&guard);
    shared_total = shared_total + 1;
    give(&guard);
    return NULL;
}

int main(void)
{
    pthread_t firstThread;
    pthread_t secondThread;

    pthread_create(&firstThread, NULL, worker, NULL);
    pthread_create(&secondThread, NULL, worker, NULL);
    pthread_join(firstThread, NULL);
    pthread_join(secondThread, NULL);
    return 0;
}
