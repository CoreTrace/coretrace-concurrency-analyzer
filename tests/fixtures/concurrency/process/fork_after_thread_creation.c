// SPDX-License-Identifier: Apache-2.0
// The child inherits the whole address space but only the calling thread. A mutex the worker
// held is copied locked, with no thread left to unlock it, and POSIX limits the child to
// async-signal-safe calls until it execs — which excludes everything below.
#include <pthread.h>
#include <stddef.h>
#include <unistd.h>

static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
static int shared_total = 0;

static void* worker(void* argument)
{
    (void)argument;
    pthread_mutex_lock(&guard);
    shared_total = shared_total + 1;
    pthread_mutex_unlock(&guard);
    return NULL;
}

int main(void)
{
    pthread_t helper;
    pthread_create(&helper, NULL, worker, NULL);

    if (fork() == 0)
    {
        // Running in the child, with a mutex that may already be locked forever.
        pthread_mutex_lock(&guard);
        shared_total = shared_total + 1;
        pthread_mutex_unlock(&guard);
        return 0;
    }

    pthread_join(helper, NULL);
    return 0;
}
