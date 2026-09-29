// SPDX-License-Identifier: Apache-2.0
// stop() records the status of its join in a global before returning it, and main reads `shared`
// only on the branch where that status reports success. The program is correct, but only a status
// the helper does nothing else with is followed through the call, so the read is still reported.
// Expected: one data race on `shared`, main's read against the worker's write.
#include <pthread.h>
static int shared;
static int last_status;
static pthread_t thread;
static void* worker(void* argument)
{
    (void)argument;
    shared += 1;
    return NULL;
}
static int stop(void)
{
    return last_status = pthread_join(thread, NULL);
}
int main(void)
{
    pthread_create(&thread, NULL, worker, NULL);
    if (stop() == 0)
        return shared;
    return last_status;
}
