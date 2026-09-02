// SPDX-License-Identifier: Apache-2.0
// Worker unit: it holds the inversion, but never names a lock primitive. Both halves of the
// proof are needed, and neither unit has both.
#include <pthread.h>
#include <stddef.h>

void take(pthread_mutex_t* lock);
void give(pthread_mutex_t* lock);

static pthread_mutex_t first_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t second_lock = PTHREAD_MUTEX_INITIALIZER;

static void* ascending(void* argument)
{
    (void)argument;
    take(&first_lock);
    take(&second_lock);
    give(&second_lock);
    give(&first_lock);
    return NULL;
}

static void* descending(void* argument)
{
    (void)argument;
    take(&second_lock);
    take(&first_lock);
    give(&first_lock);
    give(&second_lock);
    return NULL;
}

int main(void)
{
    pthread_t ascendingThread;
    pthread_t descendingThread;

    pthread_create(&ascendingThread, NULL, ascending, NULL);
    pthread_create(&descendingThread, NULL, descending, NULL);
    pthread_join(ascendingThread, NULL);
    pthread_join(descendingThread, NULL);
    return 0;
}
