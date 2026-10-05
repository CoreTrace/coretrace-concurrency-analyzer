// SPDX-License-Identifier: Apache-2.0
// As in data_race_thread_started_by_helper_after_joining_previous_no_fp.c, but a round goes on to
// start the next thread when the join of the previous one fails: that thread may still run
// (#126).
// Expected: one data race, worker against worker.
#include <pthread.h>
#include <stddef.h>

static int shared;
static int failures;

static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}

static void start(pthread_t* thread)
{
    pthread_create(thread, NULL, worker, NULL);
}

int main(void)
{
    pthread_t thread;
    start(&thread);
    for (int i = 1; i < 3; ++i)
    {
        if (pthread_join(thread, NULL) != 0)
            failures += 1;
        start(&thread);
    }
    pthread_join(thread, NULL);
    return failures;
}
