// SPDX-License-Identifier: Apache-2.0
// Defines a writer of its own, local to this unit, and runs it on an object of its own: it
// increments a counter only this unit sees. Whatever its name, it is not main.c's writer, which
// stays bound to main's task.
// Expected in project mode with main.c: no data race.
#include <pthread.h>
#include <stddef.h>

struct Task
{
    pthread_mutex_t lock;
    pthread_t thread;
};

static int mine;

static void* writer(void* argument)
{
    struct Task* held = argument;
    pthread_mutex_lock(&held->lock);
    mine += 1;
    pthread_mutex_unlock(&held->lock);
    return NULL;
}

void run_other(void)
{
    struct Task own = {PTHREAD_MUTEX_INITIALIZER, 0};
    writer(&own);
}
