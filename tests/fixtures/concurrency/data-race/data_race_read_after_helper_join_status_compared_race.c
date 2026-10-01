// SPDX-License-Identifier: Apache-2.0
// stop() returns whether its join succeeded, `status == 0`, not the status itself: main reads
// `shared` when stop() returns 0, that is when the join failed, and the worker may still be
// running there.
// Expected: one data race on `shared`, main's read against the worker's write.
#include <pthread.h>
static int shared;
static pthread_t thread;
static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}
static int stop(void)
{
    int status = pthread_join(thread, NULL);
    return status == 0;
}
int main(void)
{
    pthread_create(&thread, NULL, worker, NULL);
    if (stop() == 0)
        return shared;
    return 0;
}
