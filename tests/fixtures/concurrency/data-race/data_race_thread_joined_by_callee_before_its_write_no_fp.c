// SPDX-License-Identifier: Apache-2.0
// stop() joins the thread its caller started, then writes shared: the thread has ended there
// (#113).
// Expected: no diagnostic.
#include <pthread.h>

static int shared;

static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}

static void stop(pthread_t thread)
{
    pthread_join(thread, NULL);
    shared += 1;
}

int main(void)
{
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    stop(thread);
    return 0;
}
