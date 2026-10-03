// SPDX-License-Identifier: Apache-2.0
// Publishes the address remember() receives; the observer writes through the last one (#108).
#include <pthread.h>
static int* remembered;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_t observer_thread;
void remember(int* element)
{
    pthread_mutex_lock(&gate);
    remembered = element;
    pthread_mutex_unlock(&gate);
}
static void* observer(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&gate);
    int* seen = remembered;
    pthread_mutex_unlock(&gate);
    if (seen != NULL)
        *seen = 0;
    return NULL;
}
void start_observer(void)
{
    pthread_create(&observer_thread, NULL, observer, NULL);
}
void join_observer(void)
{
    pthread_join(observer_thread, NULL);
}
